#!/usr/bin/env python3
"""A game's music and sound as text: the commands its sound engine reads.

`gb_data.py` uses this for its `streams` command and when it puts a tree
together again.

Music in a ROM is not notes. It is a list of commands for the game's own
sound engine: play this pitch for so long, change octave, set the tempo,
call that passage, loop, end. Nothing in the ROM says where such a list
starts or what a byte of it means. `streams.json`, at the top of the tree,
says both, and somebody writes it once for a game:

    {"schema": "gbrecomp.streams", "version": 1,
     "grammars": {
      "music": {"commands": [
       {"name": "note", "first": "00-BF", "operands": [
         {"name": "pitch", "bits": [4, 7], "names": ["C_", "C#", "D_"]},
         {"name": "length", "bits": [0, 3], "add": 1}]},
       {"name": "tempo", "first": "ED", "operands": [{"name": "tempo", "byte": 1, "word": "big"}]},
       {"name": "call", "first": "FD", "operands": [
         {"name": "to", "byte": 1, "word": "little", "target": "call"}]},
       {"name": "loop", "first": "FE", "ends": {"times": 0}, "operands": [
         {"name": "times", "byte": 1},
         {"name": "to", "byte": 2, "word": "little", "target": "loop"}]},
       {"name": "end", "first": "FF", "ends": true}]},
      "drums": {"like": "music", "commands": [
       {"name": "note", "first": "00-BF", "operands": [
         {"name": "length", "bits": [0, 3], "add": 1}, {"name": "pad", "bits": [4, 7]},
         {"name": "drum", "byte": 1}]}]}},
     "starts": [
      {"table": {"bank": 2, "address": "4003", "entries": 40, "entry_size": 3, "pointer": 1},
       "grammar": {"byte": 0, "mask": "0F", "is": {"0": "music", "3": "drums"}}},
      {"at": {"bank": 2, "address": "5B00"}, "grammar": "music"}],
     "readers": [{"bank": 2, "address": "5832"}]}

A number is a number, or a string of hexadecimal digits.

**A grammar** is a list of commands. The first byte of a command says which
it is: `first` is that byte, or a range of them, and the commands are tried
in the order they are written. An operand is some bits of one byte of the
command (`byte`, counted from the first, which is 0, and `bits`, the lowest
and the highest, 0 to 7) or two bytes (`word`: `little` or `big` end
first). It is written as a number: with `add` added to it, or taken away
`from` a number, in hexadecimal with `hex`, or as one of `names` (a list,
the name of 0 first, or an object from number to name). Every bit of a
command has to be said by an operand, or be the same for all its first
bytes: that is checked when the declaration is read, and it is why text
written from bytes always goes back into the same bytes. `like` takes
another grammar's commands, with the ones named again replaced.

A `target` operand is the address of more of the stream, in the bank the
command is in or below 4000. `call` and `loop` are followed and the stream
goes on after the command; `jump` is followed and the stream ends there.
`ends` says a command ends its stream: always, or where an operand has a
value (a loop that goes round for ever). `then` names the grammar the rest
of the stream is in, for an engine with a command that changes how it
reads.

**The starts** are where streams begin: one address (`at`), or a table of
entries with a pointer in each (`pointer` is where in an entry its two
bytes are, low byte first). The grammar a start is in is a name, or told
by a byte of the table's entry (`byte`, `mask`, and `is`: the value to
the grammar), as where an entry says which channel its stream is for.

`follow` reads every start through its commands and through what they
call, jump and loop to. A byte no command of the grammar starts with stops
that stream, with a message that says where: nothing is guessed, and what
was read before it stays. Decoding goes through files of code too, since
an analysis often takes a stretch of music for instructions; such bytes
are counted and not written anywhere, as they cannot be edited.

**A passage** is a run of whole commands in one data file, from a place
something points at (a start, or a target) or where a stream comes into
the file, to the next such place. `streams` writes each region's passages
to a file of text beside it:

    # 02:4B2F Music_Town_Ch1
    02:4B2F music 9
      tempo 144
      note C_ 4
      call 02:4C10
      loop 0 02:4B2F

The line a passage starts with is its label: its address, the grammar its
commands are in, and how many bytes it has. A target is written as the
address it points at. The file is a view, read back by `build` like a file
of text or of tile numbers. A passage keeps its place: commands that make
the same number of bytes are written over the old ones; fewer bytes are
written where the passage's last command ends its stream, and what is left
after them stays as it was, since nothing reaches it; more bytes are
refused, with how many. Moving a passage that grew needs every pointer to
it turned, which is not built.

**The check against the game played.** A record made with `--data-use`
says which bytes each instruction read. The engine reads its commands
through one or two instructions, so the bytes those read should all lie in
passages, and `check` counts the ones that do, the ones that do not, and
the bytes of passages no record saw read. That is what makes a grammar
trustworthy without a disassembly.
"""
from __future__ import annotations

from bisect import bisect_right
from collections import deque
import copy
import json
from pathlib import Path
import re

SCHEMA = "gbrecomp.streams"
DECLARATION = "streams.json"    # which commands a game's sound engine has, at the top of the tree
BANK = 0x4000
TARGETS = ("call", "jump", "loop")
HEAD = re.compile(r"([0-9A-Fa-f]{2,3}):([0-9A-Fa-f]{4}) (\S+) (\d+)\s*$")
ADDRESS = re.compile(r"([0-9A-Fa-f]{2,3}):([0-9A-Fa-f]{4})$")
NUMBER = re.compile(r"-?(?:\d+|\$[0-9A-Fa-f]+|0[xX][0-9A-Fa-f]+)$")
HEXADECIMAL = re.compile(r"[0-9A-Fa-f]+$")
READ_LEAST = 16         # how many bytes an instruction has to have read to be taken for one that reads commands
READ_SHARE = 0.5        # and how many of them have to lie in passages
INTRODUCTION = """\
# The commands of {file}, a passage at a time (gb_data.py streams).
# A passage starts with a line of its own: its address, the grammar its commands are in
# (streams.json) and how many bytes it has. Its commands follow, one to a line.
# Edit the commands and leave the passage lines. A passage keeps its place: it can get
# shorter where its last command ends the stream, and it cannot get longer.
"""


class StreamsError(Exception):
    pass


def place(offset: int) -> tuple[int, int]:
    """The bank and the address a ROM offset is mapped at."""
    bank = offset // BANK
    return bank, offset if bank == 0 else BANK + offset % BANK


def at(offset: int) -> str:
    return "%02X:%04X" % place(offset)


def rom_offset(bank: int, address: int, size: int) -> int | None:
    """Where in the ROM an address is, seen from a bank: below 4000 the home bank, and from
    4000 to 7FFF that bank. None for an address that is not in the ROM so."""
    if 0 <= address < BANK:
        offset = address
    elif BANK <= address < 2 * BANK and bank > 0:
        offset = bank * BANK + address - BANK
    else:
        return None
    return offset if offset < size else None


def as_number(value, what: str) -> int:
    """A number of the declaration: a number, or a string of hexadecimal digits."""
    if type(value) is int:
        return value
    if isinstance(value, str) and HEXADECIMAL.match(value):
        return int(value, 16)
    raise StreamsError(f"{what} is {value!r}: it has to be a number, or a string of hexadecimal digits")


def fields(thing, allowed: set[str], needed: set[str], what: str) -> None:
    if not isinstance(thing, dict):
        raise StreamsError(f"{what} is not an object")
    unknown = sorted(set(thing) - allowed)
    if unknown:
        raise StreamsError(f"{what} has no such field as '{unknown[0]}'")
    missing = sorted(needed - set(thing))
    if missing:
        raise StreamsError(f"{what} lacks the field '{missing[0]}'")


def a_name(value, what: str) -> str:
    """A name that can stand as a word of a line of text and is not taken for a number or an address."""
    if not isinstance(value, str) or not value or value != "".join(value.split()) or value.startswith("#") or \
            NUMBER.match(value) or ADDRESS.match(value):
        raise StreamsError(f"{what} is {value!r}: a name is a word without a space in it, and no number")
    return value


class Operand:
    """Some bits of a byte of a command, or two bytes, and how they are written."""

    def __init__(self, spec, what: str) -> None:
        fields(spec, {"name", "byte", "bits", "word", "add", "from", "hex", "names", "target"}, {"name"}, what)
        self.name = a_name(spec["name"], f"the name of {what}")
        what = f"the operand '{self.name}' of {what.split(' of ', 1)[-1]}"
        self.byte = spec.get("byte", 0)
        self.word = spec.get("word")
        bits = spec.get("bits", [0, 7])
        if type(self.byte) is not int or not 0 <= self.byte < 16:
            raise StreamsError(f"{what} is of byte {self.byte!r}: the first byte of a command is 0")
        if self.word not in (None, "little", "big"):
            raise StreamsError(f"{what} is a word of {self.word!r} end first: 'little' or 'big'")
        if not isinstance(bits, list) or len(bits) != 2 or any(type(bit) is not int for bit in bits) or \
                not 0 <= bits[0] <= bits[1] <= 7:
            raise StreamsError(f"{what} has the bits {bits!r}: the lowest and the highest, 0 to 7")
        if self.word and "bits" in spec:
            raise StreamsError(f"{what} is a word and has bits: a word is two whole bytes")
        self.low, self.high = bits
        self.most = 0xFFFF if self.word else (1 << (self.high - self.low + 1)) - 1
        self.add = spec.get("add", 0)
        self.minus = spec.get("from")
        self.hex = spec.get("hex", False)
        self.target = spec.get("target")
        if type(self.add) is not int or (self.minus is not None and type(self.minus) is not int) or \
                type(self.hex) is not bool:
            raise StreamsError(f"{what} has an `add`, a `from` or a `hex` of the wrong type")
        if self.minus is not None and "add" in spec:
            raise StreamsError(f"{what} has both `add` and `from`: one of them")
        if self.target is not None:
            if self.target not in TARGETS:
                raise StreamsError(f"{what} is a target of the kind {self.target!r}: 'call', 'jump' or 'loop'")
            if not self.word or set(spec) & {"add", "from", "hex", "names"}:
                raise StreamsError(f"{what} is a target, which is an address: a word, written as BB:AAAA and no "
                                   f"other way")
        self.names: dict[int, str] = {}
        names = spec.get("names", [])
        if isinstance(names, list):
            listed = [(value, name) for value, name in enumerate(names) if name not in (None, "")]
        elif isinstance(names, dict):
            listed = [(as_number(value, f"a value of the names of {what}"), name) for value, name in names.items()]
        else:
            raise StreamsError(f"the names of {what} are not a list or an object")
        for value, name in listed:
            if not 0 <= value <= self.most or value in self.names:
                raise StreamsError(f"the names of {what} name {value}, which it cannot be, or name it twice")
            self.names[value] = a_name(name, f"the name of {value} of {what}")
        self.values = {name: value for value, name in self.names.items()}
        if len(self.values) != len(self.names):
            raise StreamsError(f"the names of {what} give two values the same name")

    def raw(self, data: bytes, start: int) -> int:
        if self.word == "little":
            return data[start + self.byte] | (data[start + self.byte + 1] << 8)
        if self.word == "big":
            return (data[start + self.byte] << 8) | data[start + self.byte + 1]
        return (data[start + self.byte] >> self.low) & self.most

    def shown(self, raw: int, bank: int) -> str:
        """How a value is written: a target as the address it points at, seen from `bank`."""
        if self.target:
            return f"{0 if raw < BANK else bank:02X}:{raw:04X}"
        if raw in self.names:
            return self.names[raw]
        value = self.minus - raw if self.minus is not None else raw + self.add
        if self.hex and value >= 0:
            return f"${value:0{4 if self.word else 2}X}"
        return str(value)

    def number(self, value: int) -> int | None:
        """The bits a number that was written stands for, or None where it cannot be written in them."""
        raw = self.minus - value if self.minus is not None else value - self.add
        return raw if 0 <= raw <= self.most else None

    def read(self, word: str, bank: int, what: str) -> int:
        """The other way: the bits a word of a line of text stands for."""
        if self.target:
            found = ADDRESS.match(word)
            if not found:
                raise StreamsError(f"{what}: '{self.name}' is where the stream goes on, an address written BB:AAAA, "
                                   f"and not '{word}'")
            to_bank, address = int(found.group(1), 16), int(found.group(2), 16)
            if address >= 2 * BANK or to_bank != (0 if address < BANK else bank):
                raise StreamsError(f"{what}: {word} cannot be reached from here: a stream in bank {bank:02X} goes on "
                                   f"in its own bank, from {bank:02X}:4000, or below 00:4000")
            return address
        if word in self.values:
            return self.values[word]
        if not NUMBER.match(word):
            known = f" ({', '.join(list(self.values)[:16])})" if self.values else ""
            raise StreamsError(f"{what}: '{word}' is no number, and no name '{self.name}' has{known}")
        sign, digits = (-1, word[1:]) if word.startswith("-") else (1, word)
        value = sign * (int(digits[1:], 16) if digits.startswith("$") else int(digits, 0) if digits[:2].lower() == "0x"
                        else int(digits))
        raw = self.number(value)
        if raw is None:
            ends = sorted((self.shown_number(0), self.shown_number(self.most)))
            raise StreamsError(f"{what}: '{self.name}' is {word}, and it goes from {ends[0]} to {ends[1]}")
        return raw

    def shown_number(self, raw: int) -> int:
        return self.minus - raw if self.minus is not None else raw + self.add


class Command:
    """A command of a grammar: the first bytes it has, its operands, and what it does to the stream."""

    def __init__(self, spec, what: str) -> None:
        fields(spec, {"name", "first", "operands", "ends", "then"}, {"name", "first"}, what)
        self.name = a_name(spec["name"], f"the name of {what}")
        what = f"the command '{self.name}' of {what.split(' of ', 1)[-1]}"
        first = spec["first"]
        ends = first.split("-") if isinstance(first, str) else [first]
        if len(ends) > 2:
            raise StreamsError(f"{what} has the first bytes {first!r}: one byte, or 'XX-YY'")
        self.low = as_number(ends[0], f"the first byte of {what}")
        self.high = as_number(ends[-1], f"the last first byte of {what}")
        if not 0 <= self.low <= self.high <= 255:
            raise StreamsError(f"{what} has the first bytes {first!r}, which are not bytes in order")
        if not isinstance(spec.get("operands", []), list):
            raise StreamsError(f"the operands of {what} are not a list")
        self.operands = [Operand(operand, f"operand {index + 1} of {what}")
                         for index, operand in enumerate(spec.get("operands", []))]
        if len({operand.name for operand in self.operands}) != len(self.operands):
            raise StreamsError(f"{what} has two operands of one name")
        # Which bits of each byte an operand says. No two the same bit, and after the first byte all of them.
        said: dict[int, int] = {0: 0}
        for operand in self.operands:
            for byte, mask in ((operand.byte, 0xFF), (operand.byte + 1, 0xFF)) if operand.word else \
                    ((operand.byte, operand.most << operand.low),):
                if said.get(byte, 0) & mask:
                    raise StreamsError(f"{what} has two operands of the same bits of its byte {byte}")
                said[byte] = said.get(byte, 0) | mask
        self.size = max(said) + 1
        for byte in range(1, self.size):
            if said.get(byte, 0) != 0xFF:
                raise StreamsError(f"{what} is {self.size} bytes, and no operand says what bits "
                                   f"{0xFF & ~said.get(byte, 0):08b} of its byte {byte} are: text written from it "
                                   f"would not go back into the same bytes")
        self.mask = said[0]
        self.targets = [index for index, operand in enumerate(self.operands) if operand.target]
        if len(self.targets) > 1:
            raise StreamsError(f"{what} has two targets: a command goes on in one place")
        self.target = self.targets[0] if self.targets else None
        self.then = spec.get("then")
        if self.then is not None and not isinstance(self.then, str):
            raise StreamsError(f"{what} is followed by the grammar {self.then!r}: the name of one")
        self.ends: bool | tuple[int, int] = False
        ending = spec.get("ends", False)
        if isinstance(ending, dict) and len(ending) == 1:
            name, value = next(iter(ending.items()))
            index = next((index for index, operand in enumerate(self.operands) if operand.name == name), None)
            raw = None if index is None else self.operands[index].number(as_number(value, f"what ends {what}"))
            if raw is None:
                raise StreamsError(f"{what} ends its stream where '{name}' is {value!r}, which is no operand of it "
                                   f"or no value it can have")
            self.ends = (index, raw)
        elif type(ending) is bool:
            self.ends = ending
        else:
            raise StreamsError(f"{what} ends its stream on {ending!r}: true, or one operand and its value")
        self.firsts: list[int] = []     # the first bytes that are this command in its grammar
        self.fixed = 0                  # and the bits of them that no operand says

    def raws(self, data: bytes, start: int) -> list[int]:
        return [operand.raw(data, start) for operand in self.operands]

    def ends_stream(self, raws: list[int]) -> bool:
        """Whether nothing is read after this command where it stands: it ends the stream, or jumps."""
        if self.target is not None and self.operands[self.target].target == "jump":
            return True
        if isinstance(self.ends, tuple):
            return raws[self.ends[0]] == self.ends[1]
        return self.ends

    def text(self, raws: list[int], bank: int) -> str:
        return " ".join([self.name] + [operand.shown(raw, bank) for operand, raw in zip(self.operands, raws)])

    def bytes_of(self, raws: list[int]) -> bytes:
        out = bytearray(self.size)
        out[0] = self.fixed
        for operand, raw in zip(self.operands, raws):
            if operand.word == "little":
                out[operand.byte], out[operand.byte + 1] = raw & 0xFF, raw >> 8
            elif operand.word == "big":
                out[operand.byte], out[operand.byte + 1] = raw >> 8, raw & 0xFF
            else:
                out[operand.byte] |= raw << operand.low
        return bytes(out)


class Grammar:
    """The commands of one way of reading a stream, by the first byte and by name."""

    def __init__(self, name: str, commands: list[Command]) -> None:
        self.name = name
        self.by_first: list[Command | None] = [None] * 256
        for command in reversed(commands):      # the first that takes a byte has it
            for first in range(command.low, command.high + 1):
                self.by_first[first] = command
        self.commands = []
        for command in commands:
            command.firsts = [first for first in range(command.low, command.high + 1)
                              if self.by_first[first] is command]
            if not command.firsts:
                continue        # every first byte it names is another's: it is never read
            fixed = {first & ~command.mask for first in command.firsts}
            if len(fixed) != 1:
                raise StreamsError(f"the command '{command.name}' of the grammar '{name}' has the first bytes "
                                   f"{command.firsts[0]:02X} to {command.firsts[-1]:02X}, and no operand says how "
                                   f"they differ: text written from it would not go back into the same bytes")
            command.fixed = fixed.pop()
            self.commands.append(command)
        self.by_name = {command.name: command for command in self.commands}

    def encode(self, line: str, bank: int, what: str) -> tuple[Command, list[int], bytes]:
        """A line of text as the command it is, the bits of its operands, and its bytes."""
        words = line.split()
        command = self.by_name.get(words[0])
        if command is None:
            raise StreamsError(f"{what}: the grammar '{self.name}' has no command '{words[0]}'")
        if len(words) - 1 != len(command.operands):
            takes = " ".join(operand.name for operand in command.operands) or "nothing"
            raise StreamsError(f"{what}: '{command.name}' takes {takes}, and the line has {len(words) - 1} "
                               f"{'word' if len(words) == 2 else 'words'} after it")
        raws = [operand.read(word, bank, what) for operand, word in zip(command.operands, words[1:])]
        made = command.bytes_of(raws)
        other = self.by_first[made[0]]
        if other is not command:
            raise StreamsError(f"{what}: '{line.strip()}' would start with the byte {made[0]:02X}, which is "
                               + (f"'{other.name}'" if other else "no command") + f" in the grammar '{self.name}'")
        return command, raws, made


def load_grammars(specs, what: str) -> dict[str, Grammar]:
    if not isinstance(specs, dict) or not specs:
        raise StreamsError(f"{what} has no grammars: an object of them, by name")
    loaded: dict[str, list[Command]] = {}

    def commands_of(name: str, asked: tuple[str, ...]) -> list[Command]:
        if name in loaded:
            return loaded[name]
        if name in asked:
            raise StreamsError(f"the grammar '{name}' is like itself, by way of {', '.join(asked)}")
        spec = specs.get(name)
        if spec is None:
            raise StreamsError(f"the grammar '{asked[-1]}' is like '{name}', which {what} does not have")
        fields(spec, {"commands", "like"}, set(), f"the grammar '{name}'")
        if not isinstance(spec.get("commands", []), list):
            raise StreamsError(f"the commands of the grammar '{name}' are not a list")
        own = [Command(command, f"command {index + 1} of the grammar '{name}'")
               for index, command in enumerate(spec.get("commands", []))]
        names = {command.name for command in own}
        if len(names) != len(own):
            raise StreamsError(f"the grammar '{name}' has two commands of one name")
        if "like" in spec:
            if not isinstance(spec["like"], str):
                raise StreamsError(f"the grammar '{name}' is like {spec['like']!r}: the name of a grammar")
            # A copy of each: which first bytes are a command's depends on the grammar it is in.
            own += [copy.copy(command) for command in commands_of(spec["like"], asked + (name,))
                    if command.name not in names]
        if not own:
            raise StreamsError(f"the grammar '{name}' has no commands")
        loaded[name] = own
        return own

    grammars = {a_name(name, "the name of a grammar"): Grammar(name, commands_of(name, ())) for name in specs}
    for grammar in grammars.values():
        for command in grammar.commands:
            if command.then is not None and command.then not in grammars:
                raise StreamsError(f"the command '{command.name}' of the grammar '{grammar.name}' is followed by the "
                                   f"grammar '{command.then}', which {what} does not have")
    return grammars


def a_place(spec, what: str) -> tuple[int, int]:
    bank, address = as_number(spec["bank"], f"the bank of {what}"), as_number(spec["address"], f"the address of {what}")
    if bank < 0 or not 0 <= address < 2 * BANK or (address >= BANK) != (bank > 0):
        raise StreamsError(f"{what} is at {bank:02X}:{address:04X}, which is not an address of the ROM: bank 00 is "
                           f"below 4000 and every other bank from 4000 to 7FFF")
    return bank, address


def load_declaration(path: Path) -> dict:
    """The tree's declaration of its command streams, checked: `grammars` by name, `starts` as
    they were written but with numbers for numbers, and `readers`, the instructions it says read
    commands, as (bank, address)."""
    try:
        document = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as error:
        raise StreamsError(f"cannot read {path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema") != SCHEMA or document.get("version") != 1:
        raise StreamsError(f"{path} is not a version 1 {SCHEMA} file")
    fields(document, {"schema", "version", "grammars", "starts", "readers", "why"}, {"grammars", "starts"}, str(path))
    grammars = load_grammars(document["grammars"], str(path))

    def known(name, what: str) -> str:
        if name not in grammars:
            raise StreamsError(f"{what} is in the grammar {name!r}, which {path} does not have")
        return name

    starts = []
    if not isinstance(document["starts"], list):
        raise StreamsError(f"the starts of {path} are not a list")
    for index, spec in enumerate(document["starts"]):
        what = f"start {index + 1} of {path}"
        fields(spec, {"at", "table", "grammar", "why"}, {"grammar"}, what)
        if ("at" in spec) == ("table" in spec):
            raise StreamsError(f"{what} has to be `at` an address or a `table` of them, one of the two")
        if "at" in spec:
            fields(spec["at"], {"bank", "address"}, {"bank", "address"}, f"`at` of {what}")
            bank, address = a_place(spec["at"], what)
            starts.append({"bank": bank, "address": address, "grammar": known(spec["grammar"], what)})
            continue
        table = spec["table"]
        fields(table, {"bank", "address", "entries", "entry_size", "pointer", "stream_bank"},
               {"bank", "address", "entries"}, f"the table of {what}")
        bank, address = a_place(table, f"the table of {what}")
        entries = as_number(table["entries"], f"the entries of {what}")
        entry_size = as_number(table.get("entry_size", 2), f"the size of an entry of {what}")
        pointer = as_number(table.get("pointer", 0), f"where the pointer is in an entry of {what}")
        if entries < 1 or not 0 <= pointer <= entry_size - 2:
            raise StreamsError(f"the table of {what} has {entries} entries of {entry_size} bytes with a pointer at "
                               f"byte {pointer}: at least one entry, and room in it for the pointer's two bytes")
        start = {"bank": bank, "address": address, "entries": entries, "entry_size": entry_size, "pointer": pointer}
        stream_bank = table.get("stream_bank")
        if isinstance(stream_bank, dict):
            fields(stream_bank, {"byte"}, {"byte"}, f"the bank of the streams of {what}")
            start["bank_byte"] = as_number(stream_bank["byte"], f"the byte the bank of the streams of {what} is in")
            if not 0 <= start["bank_byte"] < entry_size:
                raise StreamsError(f"the bank of the streams of {what} is in a byte the entry does not have")
        elif stream_bank is not None:
            start["stream_bank"] = as_number(stream_bank, f"the bank of the streams of {what}")
        grammar = spec["grammar"]
        if isinstance(grammar, dict):
            fields(grammar, {"byte", "mask", "is"}, {"byte", "is"}, f"the grammar of {what}")
            byte = as_number(grammar["byte"], f"the byte that tells the grammar of {what}")
            mask = as_number(grammar.get("mask", 0xFF), f"the mask of the grammar of {what}")
            if not 0 <= byte < entry_size or not 0 < mask <= 0xFF or not isinstance(grammar["is"], dict):
                raise StreamsError(f"the grammar of {what} is told by a byte the entry does not have, or by nothing")
            start["grammar"] = {"byte": byte, "mask": mask,
                                "is": {as_number(value, f"a value of the grammar of {what}"): known(name, what)
                                       for value, name in grammar["is"].items()}}
        else:
            start["grammar"] = known(grammar, what)
        starts.append(start)
    readers = []
    if not isinstance(document.get("readers", []), list):
        raise StreamsError(f"the readers of {path} are not a list")
    for spec in document.get("readers", []):
        fields(spec, {"bank", "address"}, {"bank", "address"}, f"a reader of {path}")
        readers.append((as_number(spec["bank"], "the bank of a reader"),
                        as_number(spec["address"], "the address of a reader")))
    return {"grammars": grammars, "starts": starts, "readers": readers}


def starts_of(declaration: dict, image: bytes) -> tuple[list[tuple[int, str, str]], list[str]]:
    """Where the declaration's streams start in this image, as (offset, grammar, what led there),
    and a line for each entry of a table that leads nowhere."""
    found, wrong = [], []
    for start in declaration["starts"]:
        base = rom_offset(start["bank"], start["address"], len(image))
        here = f"{start['bank']:02X}:{start['address']:04X}"
        if "entries" not in start:
            if base is None:
                wrong.append(f"the start at {here} is not in the ROM")
            else:
                found.append((base, start["grammar"], f"the start at {here}"))
            continue
        if base is None or base // BANK != (base + start["entries"] * start["entry_size"] - 1) // BANK:
            wrong.append(f"the table at {here} is not in the ROM, or does not end in its bank")
            continue
        for index in range(start["entries"]):
            entry = base + index * start["entry_size"]
            what = f"entry {index + 1} of the table at {here} ({at(entry)})"
            grammar = start["grammar"]
            if isinstance(grammar, dict):
                value = image[entry + grammar["byte"]] & grammar["mask"]
                if value not in grammar["is"]:
                    wrong.append(f"{what} has {value:02X} where its grammar is told, and {DECLARATION} names no "
                                 f"grammar for that")
                    continue
                grammar = grammar["is"][value]
            bank = image[entry + start["bank_byte"]] if "bank_byte" in start \
                else start.get("stream_bank", start["bank"])
            address = image[entry + start["pointer"]] | (image[entry + start["pointer"] + 1] << 8)
            offset = rom_offset(bank, address, len(image))
            if offset is None:
                wrong.append(f"{what} points at {bank:02X}:{address:04X}, which is not in the ROM")
                continue
            found.append((offset, grammar, what))
    return found, wrong


class Followed:
    """What following the starts found: each command by where it is, as (the command, its
    grammar, the bits of its operands); the places something points at; where a stream stopped
    and why; and the places two grammars both read."""

    def __init__(self, size: int) -> None:
        self.commands: dict[int, tuple[Command, Grammar, list[int]]] = {}
        self.covered = bytearray(size)      # 1 at the first byte of a command, 2 at its others
        self.labels: set[int] = set()
        self.starts: list[tuple[int, str, str]] = []
        self.wrong: list[str] = []
        self.stops: dict[int, str] = {}
        self.twice: dict[int, tuple[str, str]] = {}

    def whole(self, start: int) -> bool:
        """Whether everything a start leads to was read to where its commands end it."""
        seen, queue = {start}, [start]
        while queue:
            offset = queue.pop()
            if offset not in self.commands:
                return False
            command, _, raws = self.commands[offset]
            after = [] if command.ends_stream(raws) else [offset + command.size]
            if command.target is not None:
                after.append(rom_offset(offset // BANK, raws[command.target], len(self.covered)))
            for other in after:
                if other not in seen:
                    seen.add(other)
                    queue.append(other)
        return True


def follow(declaration: dict, image: bytes) -> Followed:
    """Every start read through its commands, and through what they call, jump and loop to. A
    stream stops, with a line that says where and why, at a byte no command of its grammar
    starts with, at a command that leaves its bank or points out of the ROM, and where it meets
    bytes that were read another way. Nothing is guessed, and what was read before a stop stays."""
    grammars = declaration["grammars"]
    found = Followed(len(image))
    found.starts, found.wrong = starts_of(declaration, image)
    queue = deque((offset, grammars[grammar], what) for offset, grammar, what in found.starts)
    while queue:
        offset, grammar, origin = queue.popleft()
        found.labels.add(offset)
        while True:
            if found.covered[offset] == 1:
                other = found.commands[offset][1]
                if other is not grammar:        # read in two grammars: the first one is the one written
                    found.twice.setdefault(offset, (other.name, grammar.name))
                break
            if found.covered[offset] == 2:
                found.stops.setdefault(offset, f"the stream from {origin} leads into the middle of a command that "
                                               f"was read from elsewhere")
                break
            command = grammar.by_first[image[offset]]
            if command is None:
                found.stops.setdefault(offset, f"no command of the grammar '{grammar.name}' starts with "
                                               f"{image[offset]:02X} (the stream from {origin})")
                break
            end = offset + command.size
            if end > min(len(image), (offset // BANK + 1) * BANK):
                found.stops.setdefault(offset, f"the command '{command.name}' here would leave its bank (the stream "
                                               f"from {origin})")
                break
            if any(found.covered[offset + 1:end]):
                found.stops.setdefault(offset, f"the command '{command.name}' here would run into bytes that were "
                                               f"read from elsewhere (the stream from {origin})")
                break
            raws = command.raws(image, offset)
            target = None
            if command.target is not None:
                target = rom_offset(offset // BANK, raws[command.target], len(image))
                if target is None:
                    found.stops.setdefault(offset, f"the command '{command.name}' here points at "
                                                   f"{raws[command.target]:04X}, which is not in the ROM as bank "
                                                   f"{offset // BANK:02X} sees it (the stream from {origin})")
                    break
            found.commands[offset] = (command, grammar, raws)
            found.covered[offset] = 1
            for other in range(offset + 1, end):
                found.covered[other] = 2
            if target is not None:
                queue.append((target, grammar, f"the '{command.name}' at {at(offset)}"))
            if command.ends_stream(raws):
                break
            if command.then is not None:
                grammar = grammars[command.then]
            offset = end
            if offset >= len(image) or offset % BANK == 0:
                found.stops.setdefault(offset, f"the stream from {origin} runs off the end of its bank")
                break
    return found


def passages_of(found: Followed, regions: list[dict]) -> tuple[dict[int, list[list]], dict[str, int]]:
    """The passages of each data region, by the region's place in `regions`, as [offset in the
    region, size, grammar]: runs of whole commands, cut where something points, where the
    grammar changes and where a stream stops being one run. Also how many bytes of commands are
    in files of code or the header (`pinned`), and how many are of commands that lie in two
    files (`split`); neither can be in a view."""
    starts = [region["offset"] for region in regions]
    passages: dict[int, list[list]] = {}
    counts = {"pinned": 0, "split": 0}
    last = None                 # the passage the command before is in, and where that command ends
    for offset in sorted(found.commands):
        command, grammar, _ = found.commands[offset]
        index = bisect_right(starts, offset) - 1
        region = regions[index]
        end = offset + command.size
        if end > region["offset"] + region["size"]:
            counts["split"] += command.size
            last = None
            continue
        if region["kind"] != "data":
            counts["pinned"] += command.size
            last = None
            continue
        if last is not None and last[0] == index and last[1] == offset and last[2][2] == grammar.name and \
                offset not in found.labels:
            last[2][1] += command.size
        else:
            passage = [offset - region["offset"], command.size, grammar.name]
            passages.setdefault(index, []).append(passage)
            last = [index, end, passage]
        last[1] = end
    return passages, counts


def passages_text(region: dict, passages: list[list], found: Followed) -> str:
    """A region's passages as a file of text: for each its line, then its commands."""
    lines = INTRODUCTION.format(file=region["file"]).splitlines()
    names: dict[int, list[str]] = {}
    for offset, name in region.get("labels", ()):
        names.setdefault(offset, []).append(name)
    bank, base = region["bank"], region["offset"]
    for start, size, grammar in passages:
        for name in names.get(start, ()):
            lines.append(f"# {bank:02X}:{region['address'] + start:04X} {name}")
        lines.append(f"{bank:02X}:{region['address'] + start:04X} {grammar} {size}")
        offset = base + start
        while offset < base + start + size:
            command, _, raws = found.commands[offset]
            lines.append("  " + command.text(raws, bank))
            offset += command.size
        if offset in found.stops:
            lines.append(f"# The stream stops after this, at {at(offset)}: {found.stops[offset]}.")
    return "\n".join(lines) + "\n"


def read_passages(name: str, text: str) -> list[tuple[int, int, str, int, list[tuple[int, str]]]]:
    """A file of passages as (bank, address, grammar, size, its lines of commands with the
    number of each in the file). A line that starts with # is a comment."""
    passages: list[tuple[int, int, str, int, list[tuple[int, str]]]] = []
    for number, line in enumerate(text.split("\n"), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        head = HEAD.match(line)
        if head:
            passages.append((int(head.group(1), 16), int(head.group(2), 16), head.group(3), int(head.group(4)), []))
        elif not passages:
            raise StreamsError(f"{name}, line {number}: '{line.strip()}' is before the first passage's line")
        else:
            passages[-1][4].append((number, line))
    return passages


def said(name: str, text: str, region: dict, passages: list[tuple], grammars: dict[str, Grammar],
         content: bytes) -> list[tuple[bytes, int]]:
    """The bytes each of a view's passages has by what its file of text says now, each as long
    as the passage, and how many bytes shorter its commands have become. A passage that got
    shorter keeps what was after its commands, and has to end its stream with its last command;
    one that got longer is refused."""
    written = read_passages(name, text)
    bank = region["bank"]
    wanted = [(bank, region["address"] + offset, grammar, size) for offset, size, grammar in passages]
    if [passage[:4] for passage in written] != wanted:
        odd = next((index for index, passage in enumerate(written) if index >= len(wanted) or
                    passage[:4] != wanted[index]), len(written))
        is_there = ("has '%02X:%04X %s %d'" % written[odd][:4]) if odd < len(written) else "ends"
        was_there = ("the passage '%02X:%04X %s %d' was" % wanted[odd]) if odd < len(wanted) else "there was none"
        raise StreamsError(f"{name} {is_there} where {was_there}: the lines that passages start with are not to "
                           f"be edited, and `streams --force` writes the file again")
    out = []
    for (offset, size, grammar_name), (_, address, _, _, lines) in zip(passages, written):
        where = f"the passage at {bank:02X}:{address:04X}"
        grammar = grammars.get(grammar_name)
        if grammar is None:
            raise StreamsError(f"{name}: {where} is in the grammar '{grammar_name}', which {DECLARATION} no longer "
                               f"has")
        made = bytearray()
        last = None
        for number, line in lines:
            what = f"{name}, line {number}"
            if last is not None and last[0].then is not None:
                raise StreamsError(f"{what}: '{last[0].name}' on the line before changes the grammar the stream is "
                                   f"read in, and can only be the last command of a passage")
            command, raws, encoded = grammar.encode(line, bank, what)
            made += encoded
            last = (command, raws)
        if last is None:
            raise StreamsError(f"{name}: {where} has no command left: a passage keeps its place, and at least one "
                               f"command")
        if len(made) > size:
            raise StreamsError(f"{name}: {where} is {len(made)} bytes now and there is room for {size}: "
                               f"{len(made) - size} too many. A passage cannot grow where it is, and moving one is "
                               f"not built")
        if len(made) < size and not last[0].ends_stream(last[1]):
            raise StreamsError(f"{name}: {where} is {size - len(made)} {'byte' if size - len(made) == 1 else 'bytes'} "
                               f"shorter than the {size} it had, and its last command, '{last[0].name}', does not "
                               f"end the stream: what follows would be read from the wrong place. Keep its length, "
                               f"or end it")
        out.append((bytes(made) + content[offset + len(made):offset + size], size - len(made)))
    return out


def readers_found(regions: list[dict], covered: bytearray) -> list[tuple[int, int]]:
    """The instructions that read commands, by what `uses` wrote: those that read first at least
    READ_LEAST bytes, more than READ_SHARE of them in passages."""
    counts: dict[tuple[int, int], list[int]] = {}
    for region in regions:
        for seen in region.get("seen", ()):
            count = counts.setdefault((seen["read_at"][0], seen["read_at"][1]), [0, 0])
            first = region["offset"] + seen["offset"]
            count[0] += seen["size"]
            count[1] += seen["size"] - covered.count(0, first, first + seen["size"])
    return sorted(reader for reader, (read, inside) in counts.items()
                  if read >= READ_LEAST and inside > READ_SHARE * read)


def check(regions: list[dict], found: Followed, readers: list[tuple[int, int]]) -> dict:
    """What the records say of the passages, for a tree `uses` has written into: of the bytes the
    instructions in `readers` read first, how many lie in decoded commands (`inside`) and how
    many do not (`outside`, with the runs of them in `missed` as (offset, size)); and of the
    decoded bytes that are in data files (`decoded`), how many another instruction read first
    (`by_others`, counted by instruction in `others`) and how many no record saw read
    (`unseen`)."""
    wanted = set(readers)
    covered = found.covered
    result = {"inside": 0, "outside": 0, "missed": [], "decoded": 0, "by_others": 0, "unseen": 0, "others": {}}
    for region in regions:
        if region["kind"] != "data":
            continue
        base = region["offset"]
        decoded = region["size"] - covered.count(0, base, base + region["size"])
        result["decoded"] += decoded
        read = 0
        for seen in region.get("seen", ()):
            first = base + seen["offset"]
            inside = seen["size"] - covered.count(0, first, first + seen["size"])
            read += inside
            reader = (seen["read_at"][0], seen["read_at"][1])
            if reader in wanted:
                result["inside"] += inside
                result["outside"] += seen["size"] - inside
                for offset in range(first, first + seen["size"]):
                    if not covered[offset]:
                        if result["missed"] and sum(result["missed"][-1]) == offset:
                            result["missed"][-1][1] += 1
                        else:
                            result["missed"].append([offset, 1])
            elif inside:
                result["by_others"] += inside
                result["others"][reader] = result["others"].get(reader, 0) + inside
        result["unseen"] += decoded - read
    return result
