#!/usr/bin/env python3
"""Data that grows by moving: the pointers a game reads its data through, and
a thing put into free space with those pointers turned to it.

`gb_data.py` uses this for its `pointers` and `free` commands, and when it
puts a tree together again with a string in it that was made longer.

A file of the tree keeps its size and its place because nothing says who
reads it. Whatever reads it has its address, and would go on reading the old
place. What a played game does show is who reads it through a pointer: a
game run with `--data-use-pointers` as well as `--data-use` writes into its
record, for every byte of the ROM it read, whether the address it was read
through was made of two bytes of the ROM, and which two. They are a pointer
in a table, which the game read as data, or the operand of an instruction
(`ld hl, nn`). An address counts as made of them for as long as only plain
numbers are added to it, which is what stepping along and indexing do.

`find_things` cuts each bank where its files start and where pointers lead.
A thing is the bytes from where one pointer leads to where the next one
does, or to the end of its file; where the game read on through one pointer
past the place another leads to, the two are one thing. A thing can move if
every read of it in the records came through pointers that are bytes of
data, used for nothing else: it is then copied somewhere else, those
pointers are turned to the copy, and what read it reads the copy. A thing
an instruction's operand leads to cannot move, since the operand is code
the game's C was compiled from, and no image may change it: unless the
game's source names that operand. Lifted source kept in a folder
(`gb_lift.py source --data-tree`) writes `ld hl, $43DE` as `ctx->hl =
BaseStats;`, and says which operands it names (`pointers --named`). Such an
operand is a pointer like one in data, with one difference: it is turned
in the source, not in the image. `move` leaves its bytes alone and hands
back where the name leads now, and `build --moved-header` writes that as a
header the game is compiled with. Nor can a thing move
whose pointer the game also reckons with, or compares: the record marks the
bytes that were numbers to the game, and an address that is a number as
well cannot be turned without changing what comes of the reckoning.

Moving is a copy (`move`). The old bytes stay where they are, so whatever no
record saw reading them still finds them, and shows what was there before.
Room for the copies has to be declared, in `free.json` at the top of the
tree: `propose_free` finds the empty ends of banks, and nothing is ever
written anywhere else. A thing stays in its bank, since the two bytes of an
address are turned and not the byte that says the bank.

All of it is what the records show and no more. A pointer nobody recorded
goes on leading to the old place. What follows a string in its thing moves
along with it, which is right where the game comes to it by reading on and
wrong where it counts from the thing's start. A thing ends where the next
recorded pointer leads, so a game that reads past there through a pointer
it was never recorded reading that far through reads what lies after the
copy. And the record sees reckoning, not every way a pointer can decide
what the game does: `move` with `everything` set is the check, on a game
played from the image it makes.
"""
from __future__ import annotations

import bisect
import difflib
import json
from pathlib import Path

USE_SCHEMA = "gbrt.data-use"
FREE_SCHEMA = "gbrecomp.free-space"
FREE = "free.json"              # the free space a tree declares, at its top
BANK = 0x4000
REACHED = {"data": 1, "operand": 2, "other": 4}     # how a byte was reached, as the record names it
FREE_LEAST = 32         # a run of fewer bytes at the end of a bank is not proposed
FREE_MARGIN = 16        # and its first bytes are left alone: what lies before it may end in the same value
LISTED = 12             # how many lines of a kind a report prints
THING_FIELDS = {"offset", "size", "bank", "address", "read", "pointers", "moves", "why", "pinned"}
POINTER_FIELDS = {"low", "high", "value", "at", "operand", "named", "runs"}
NAMED_SCHEMA = "gbrecomp.lift-named-operands"
# Why a thing cannot move, in the order the report counts them.
KINDS = {
    "operand": "are reached through an instruction's operand that no source names, which is code and stays as "
               "it is",
    "other": "were read through an address that no two bytes of the ROM made",
    "stray": "were read through a pointer that does not lead to them",
    "code": "have a pointer to them in a file of code, which no image may change",
    "used": "have a pointer to them that the game uses for something else as well",
    "spelled": "have their address spelled by an operand that no source names, which no record saw reading them",
    "shared": "have a pointer to them that shares a byte with another, or is read with two banks",
}


class PointersError(Exception):
    pass


def place(offset: int) -> tuple[int, int]:
    """The bank and the address a ROM offset is mapped at."""
    bank = offset // BANK
    return bank, offset if bank == 0 else BANK + offset % BANK


def where(offset: int) -> str:
    bank, address = place(offset)
    return f"{bank:02X}:{address:04X}"


def times(count: int, one: str, many: str | None = None) -> str:
    return f"{count:,} {one if count == 1 else many or one + 's'}"


def load_links(paths: list[Path], size: int, checksum: int) -> tuple[dict[tuple, list[int]], bytearray, bytearray]:
    """What the records say of the addresses the ROM was read through.

    The pairs of bytes that made them, by (low, high, operand, bank): for
    each, the lowest and the highest offset read through it, the least and
    the most that had been added to it, and how many reads. For each byte of
    the ROM how it was reached, as bits of REACHED. And for each byte whether
    it was a number to the game: compared, or put through arithmetic that an
    address is not. Several records are several runs of the game, and add up."""
    links: dict[tuple, list[int]] = {}
    reached = bytearray(size)
    numbers = bytearray(size)
    for path in paths:
        try:
            record = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise PointersError(f"cannot read the record {path}: {error}") from error
        if not isinstance(record, dict) or record.get("schema") != USE_SCHEMA or record.get("version") != 1:
            raise PointersError(f"{path} is not a version 1 {USE_SCHEMA} record")
        if record["rom"]["size"] != size or record["rom"]["global_checksum"] != checksum:
            raise PointersError(f"{path} was recorded from another ROM")
        section = record.get("pointers")
        if not isinstance(section, dict):
            raise PointersError(f"{path} was recorded without --data-use-pointers: it does not say which bytes "
                                f"made the addresses the game read through")
        for link in section["links"]:
            fields = [link[field] for field in ("low", "high", "bank", "first", "last", "least", "most", "reads")]
            if any(type(number) is not int for number in fields) or type(link["operand"]) is not bool or \
                    not all(0 <= link[field] < size for field in ("low", "high", "first", "last")):
                raise PointersError(f"{path}: a pair of bytes that made an address is not inside the ROM")
            key = (link["low"], link["high"], link["operand"], link["bank"])
            entry = links.get(key)
            if entry is None:
                links[key] = fields[3:]
            else:
                entry[0] = min(entry[0], link["first"])
                entry[1] = max(entry[1], link["last"])
                entry[2] = min(entry[2], link["least"])
                entry[3] = max(entry[3], link["most"])
                entry[4] += link["reads"]
        for run in section["reached"]:
            bits = 0
            for name in run["through"]:
                if name not in REACHED:
                    raise PointersError(f"{path}: a byte cannot be reached through '{name}'")
                bits |= REACHED[name]
            if not 0 <= run["offset"] <= run["offset"] + run["size"] <= size:
                raise PointersError(f"{path}: a run of bytes that were reached leaves the ROM at offset "
                                    f"{run['offset']:#x}")
            for offset in range(run["offset"], run["offset"] + run["size"]):
                reached[offset] |= bits
        for run in section.get("numbers", ()):
            if not 0 <= run["offset"] <= run["offset"] + run["size"] <= size:
                raise PointersError(f"{path}: a run of bytes that were numbers leaves the ROM at offset "
                                    f"{run['offset']:#x}")
            numbers[run["offset"]:run["offset"] + run["size"]] = b"\x01" * run["size"]
    return links, reached, numbers


def resolve(links: dict[tuple, list[int]], image: bytes) -> list[dict]:
    """Each pair of bytes with where it leads: the place the two bytes spell,
    in the bank of what was read through them. `bad` says why a pair does not
    lead to what was read through it, where it does not."""
    resolved = []
    for (low, high, operand, bank), (first, last, least, most, reads) in sorted(links.items()):
        value = image[low] | (image[high] << 8)
        to, bad = None, None
        if value < BANK:
            to = value
            if bank != 0:
                bad = "leads into the first bank, and what was read through it is in another"
        elif value < 2 * BANK:
            to = bank * BANK + value - BANK
            if bank == 0:
                bad = "leads into a bank that is switched, and what was read through it is in the first"
        else:
            bad = "does not lead into the ROM"
        if bad is None and (to + least != first or to + most != last):
            bad = "does not say now what it said when the game was recorded"
        resolved.append({"low": low, "high": high, "operand": operand, "bank": bank, "value": value, "to": to,
                         "first": first, "last": last, "reads": reads, "bad": bad})
    return resolved


def runs_of(flags: bytes) -> list[list[int]]:
    """The runs of bytes that are not zero, as [first, how many]."""
    runs: list[list[int]] = []
    for index, flag in enumerate(flags):
        if not flag:
            continue
        if runs and runs[-1][0] + runs[-1][1] == index:
            runs[-1][1] += 1
        else:
            runs.append([index, 1])
    return runs


class Named(dict):
    """The operands the game's source names, as {offset of the operand's low
    byte: the name of what it leads to}, with two things more: `runs`, how
    many bytes each such operand's thing runs for the source ({offset: size},
    where the source knows), and `unnamed`, the other operands of the game
    that spell an address in the ROM, as (offset, the bank the instruction is
    in, the address)."""

    def __init__(self) -> None:
        super().__init__()
        self.runs: dict[int, int] = {}
        self.unnamed: list[tuple[int, int, int]] = []


def load_named(path: Path, sha1: str) -> Named:
    """The file `gb_lift.py build` writes into a source folder made with
    `source --data-tree` (`Named`)."""
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise PointersError(f"cannot read {path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema") != NAMED_SCHEMA or document.get("version") != 1 or \
            not isinstance(document.get("operands"), list):
        raise PointersError(f"{path} is not a version 1 {NAMED_SCHEMA} file")
    if document.get("rom", {}).get("sha1") not in (None, sha1):
        raise PointersError(f"{path} is of the source of another ROM")
    named = Named()
    for entry in document["operands"]:
        if not isinstance(entry, dict) or type(entry.get("offset")) is not int or \
                type(entry.get("name")) is not str or not entry["name"] or type(entry.get("size", 0)) is not int:
            raise PointersError(f"{path}: an operand is an offset and a name")
        named[entry["offset"]] = entry["name"]
        if entry.get("size", 0) > 0:
            named.runs[entry["offset"]] = entry["size"]
    for entry in document.get("unnamed", ()):
        if not isinstance(entry, dict) or any(type(entry.get(field)) is not int for field in ("offset", "bank", "value")):
            raise PointersError(f"{path}: an operand that is not named is an offset, a bank and what it says")
        named.unnamed.append((entry["offset"], entry["bank"], entry["value"]))
    return named


def find_things(regions: list[dict], image: bytes, links: dict[tuple, list[int]], reached: bytearray,
                numbers: bytearray, uses: dict[int, tuple],
                named: Named | None = None) -> tuple[list[dict], list[str], dict]:
    """The things the records reached in the tree's data files, as the manifest
    keeps them, with for each the kind of reason it cannot move ("moves" if it
    can), and some counts for the report.

    A bank is cut at the start of each file and at each place a pointer leads
    to. A cut that the game read across, on through a pointer that leads to
    somewhere in front of it, is no cut: what is on both sides is one thing.
    So a thing ends where the next pointer leads or its file ends, and is in
    more than one file only where the game read on from one into the next.

    `uses` is what the same records say became of each byte that was read
    (`gb_data.load_uses`): a pointer that the game also jumps through, or
    draws, is not only a pointer. Nor is one of whose bytes `numbers` says
    that the game compared it or reckoned with it: an address the game takes
    the place of something from, by taking a table's start from it, is a
    number as well, and turning it would change the number.

    `named` is the operands the game's source names (`load_named`). Such a
    one is turned in the source, so it does not hold a thing in its place,
    and the rest is asked of it as of a pointer in data, but for where it
    lies: it lies in code, and that is where the source has it from. And a
    thing whose address any other operand of the game spells stays, whether
    or not a record saw that operand read it: the source knows every operand
    there is, as no record does."""
    named = named or Named()
    starts = [region["offset"] for region in regions]
    # Where the operands no source names lead, if they are addresses: by ROM offset. One in the
    # first bank can mean any bank, so it is counted for the place it spells in each.
    spelled_low: set[int] = set()
    spelled_any: set[int] = set()   # an address from 4000 up, spelled by code of the first bank
    for _, bank, value in named.unnamed:
        if value < BANK:
            spelled_low.add(value)
        elif bank == 0:
            spelled_any.add(value)
        else:
            spelled_low.add(bank * BANK + value - BANK)
    resolved = resolve(links, image)
    strays = [link for link in resolved if link["bad"]]
    leading: dict[int, list[dict]] = {}          # a bank -> the pairs that lead into it
    for link in resolved:
        if not link["bad"]:
            leading.setdefault(link["to"] // BANK, []).append(link)
    # A byte that is half of two different pairs, and a pair read through with two banks mapped,
    # cannot be turned for one without the other going wrong.
    pairs_at: dict[int, set[tuple]] = {}
    banks_of: dict[tuple, set[int]] = {}
    for link in resolved:
        pair = (link["low"], link["high"], link["operand"])
        pairs_at.setdefault(link["low"], set()).add(pair)
        pairs_at.setdefault(link["high"], set()).add(pair)
        banks_of.setdefault(pair, set()).add(link["bank"])

    def judge(flags: int, others: int, entries: list[dict], strayed: list[dict]) -> tuple[str, str]:
        if flags & REACHED["other"]:
            return "other", (f"{times(others, 'of its bytes was', 'of its bytes were')} read through an address "
                             f"that no two bytes of the ROM made")
        if strayed:
            stray = strayed[0]
            return ("operand" if stray["operand"] else "stray",
                    f"it was read through the {'operand' if stray['operand'] else 'pointer'} at "
                    f"{where(stray['low'])}, which {stray['bad']}")
        operands = [link for link in entries if link["operand"] and link["low"] not in named]
        if operands or (flags & REACHED["operand"] and not any(link["operand"] for link in entries)):
            return "operand", ("it is reached through an instruction's operand, at "
                               + ", ".join(where(link["low"]) for link in operands[:3])
                               + (f" and {len(operands) - 3} more places" if len(operands) > 3 else "")
                               + ": that is code, and stays as it is, and no source names it")
        for link in entries:
            pair = (link["low"], link["high"], link["operand"])
            if not link["operand"]:
                for offset in (link["low"], link["high"]):
                    if regions[bisect.bisect_right(starts, offset) - 1]["kind"] != "data":
                        return "code", (f"the pointer to it at {where(link['low'])} lies in a file of code, which "
                                        f"no image may change")
                also: set[str] = set()
                for offset in (link["low"], link["high"]):
                    if offset in uses:
                        also |= (set(uses[offset][0]) | set(uses[offset][1])) - {"pointer"}
                if also:
                    return "used", (f"the pointer to it at {where(link['low'])} is not only a pointer: the game "
                                    f"also used it for {', '.join(sorted(also))}")
            elif reached[link["low"]] or reached[link["high"]]:
                # The game read its own instruction: it copies that code somewhere and runs
                # the copy, or adds it up. The source turns the instruction, not the copy.
                return "used", (f"the operand that leads to it at {where(link['low'])} is not only an operand: "
                                f"the game also reads it as data, as code it copies is read, and a copy keeps "
                                f"the address the ROM has")
            if numbers[link["low"]] or numbers[link["high"]]:
                if link["operand"]:
                    return "used", (f"the operand that leads to it at {where(link['low'])} is not only a pointer: "
                                    f"the game also compared it, reckoned with it as a number, or jumped to what "
                                    f"it made")
                return "used", (f"the pointer to it at {where(link['low'])} is not only a pointer: the game also "
                                f"compared it or reckoned with it as a number")
            if len(pairs_at[link["low"]]) > 1 or len(pairs_at[link["high"]]) > 1:
                return "shared", f"the pointer to it at {where(link['low'])} shares a byte with another pointer"
            if len(banks_of[pair]) > 1:
                return "shared", (f"the pointer to it at {where(link['low'])} was read through with "
                                  f"{len(banks_of[pair])} different banks mapped")
        if not entries:
            return "stray", "no pointer that leads to it was recorded"
        return "moves", ""

    def spelled(places: set[int]) -> int | None:
        """One of these places that an operand no source names spells too. Asked of the places
        pointers lead to, not of every byte: a number that happens to be an address somewhere
        inside a picture is a number, and the operand nobody saw at work nearly always leads
        where the ones that were seen lead."""
        for offset in sorted(places):
            if offset in spelled_low or (offset >= BANK and BANK + offset % BANK in spelled_any):
                return offset
        return None

    things: list[dict] = []
    kinds: list[str] = []
    counts = {"links": len(resolved), "operands": sum(1 for link in resolved if link["operand"]),
              "strays": len(strays), "in_code": 0, "read": 0}
    for bank_start in range(0, len(image), BANK):
        bank_end = min(bank_start + BANK, len(image))
        if not any(reached[bank_start:bank_end]):
            continue
        entries = sorted(leading.get(bank_start // BANK, ()), key=lambda link: (link["to"], link["low"], link["high"]))
        # What the game read through each pair, from where the pair leads: joined where they overlap.
        spans: list[list[int]] = []
        for low, high in sorted((min(link["to"], link["first"]), max(link["to"], link["last"])) for link in entries):
            if spans and low <= spans[-1][1]:
                spans[-1][1] = max(spans[-1][1], high)
            else:
                spans.append([low, high])
        span_starts = [span[0] for span in spans]
        files = starts[bisect.bisect_right(starts, bank_start):bisect.bisect_left(starts, bank_end)]
        cuts = [bank_start]
        for cut in sorted({link["to"] for link in entries} | set(files)):
            before = bisect.bisect_left(span_starts, cut) - 1      # the last span that starts in front of it
            if cut > bank_start and not (before >= 0 and cut <= spans[before][1]):
                cuts.append(cut)
        for first, stop in zip(cuts, cuts[1:] + [bank_end]):
            flags = bytes(reached[first:stop])
            if not any(flags):
                continue
            # The parts of it that are not in data files: no image may change those.
            pinned = []
            number = bisect.bisect_right(starts, first) - 1
            while number < len(regions) and regions[number]["offset"] < stop:
                region = regions[number]
                if region["kind"] != "data":
                    low = max(first, region["offset"])
                    pinned.append([low - first, min(stop, region["offset"] + region["size"]) - low])
                number += 1
            in_code = sum(1 for low, size in pinned for flag in flags[low:low + size] if flag)
            if in_code == sum(1 for flag in flags if flag):
                counts["in_code"] += in_code     # none of what was read of it is data to the tree
                continue
            mine = [link for link in entries if first <= link["to"] < stop]
            strayed = [link for link in strays if link["first"] < stop and first <= link["last"]]
            every = 0
            for flag in set(flags):
                every |= flag
            kind, why = judge(every, sum(1 for flag in flags if flag & REACHED["other"]), mine, strayed)
            if kind == "moves":
                # Asked of every thing once the source is known, whichever kind of pointer
                # leads to it: the operand that nobody saw is as blind to a turned table entry.
                at = spelled({first} | {link["to"] for link in mine})
                if at is not None:
                    kind, why = "spelled", (f"an operand that no source names says {place(at)[1]:04X}, where a "
                                            f"pointer to it leads: nothing saw that operand read it, and it would "
                                            f"go on reading the old place")
            bank, address = place(first)
            thing = {"offset": first, "size": stop - first, "bank": bank, "address": address, "read": runs_of(flags),
                     "pointers": [dict({"low": link["low"], "high": link["high"], "value": link["value"],
                                        "at": link["to"] - first}, **({"operand": True} if link["operand"] else {}),
                                       **({"named": named[link["low"]]} if link["operand"] and link["low"] in named
                                          else {}),
                                       **({"runs": named.runs[link["low"]]} if link["operand"] and
                                          link["low"] in named.runs else {}))
                                  for link in mine],
                     "moves": kind == "moves"}
            if pinned:
                thing["pinned"] = pinned
            if why:
                thing["why"] = why
            things.append(thing)
            kinds.append(kind)
            counts["read"] += sum(size for _, size in thing["read"])
    return things, kinds, counts


def report(things: list[dict], kinds: list[str], counts: dict, records: int) -> list[str]:
    """What `pointers` prints."""
    lines = [f"{times(records, 'record')}: {times(counts['links'], 'pair of bytes', 'pairs of bytes')} made addresses "
             f"the game read its ROM through, {counts['links'] - counts['operands']:,} read as data and "
             f"{counts['operands']:,} an instruction's operand"]
    if counts["strays"]:
        lines.append(f"  {times(counts['strays'], 'of them does', 'of them do')} not lead to what was read through "
                     f"{'it' if counts['strays'] == 1 else 'them'}: something else was added that the record did not "
                     f"see as a number")
    lines.append(f"  {times(len(things), 'thing')} reached in data files, {counts['read']:,} bytes of them read")
    moving = [thing for thing, kind in zip(things, kinds) if kind == "moves"]
    turned = sum(1 for thing in moving for pointer in thing["pointers"] if "named" not in pointer)
    by_name = len({pointer["low"] for thing in moving for pointer in thing["pointers"] if "named" in pointer})
    through = [f"{times(turned, 'pointer')} that {'lies' if turned == 1 else 'lie'} in data"] if turned or not by_name \
        else []
    if by_name:
        through.append(f"{times(by_name, 'operand')} the game's source has by name")
    lines.append(f"  {len(moving):6,d} can move ({sum(thing['size'] for thing in moving):,} bytes): every read of them "
                 f"came through {' and '.join(through)}")
    for kind, said in KINDS.items():
        count = kinds.count(kind)
        if count:
            lines.append(f"  {count:6,d} {said}")
    if counts["in_code"]:
        lines.append(f"  {counts['in_code']:,} bytes more were read in files of code or the header, which are not "
                     f"data to the tree")
    return lines


def checked_things(manifest: dict) -> list[dict] | None:
    """The manifest's things, which have to be ones `pointers` wrote, or None where it has none."""
    things = manifest.get("things")
    if things is None:
        return None
    if not isinstance(things, list):
        raise PointersError("the manifest's things are not a list")
    size = manifest["rom"]["size"]
    end = 0
    for thing in things:
        if not isinstance(thing, dict):
            raise PointersError("a thing of the manifest is not an object")
        unknown = sorted(set(thing) - THING_FIELDS)
        missing = sorted(THING_FIELDS - {"why", "pinned"} - set(thing))
        if unknown:
            raise PointersError(f"a thing of the manifest has no such field as '{unknown[0]}'")
        if missing:
            raise PointersError(f"a thing of the manifest lacks the field '{missing[0]}'")
        if any(type(thing[field]) is not int for field in ("offset", "size", "bank", "address")) or \
                type(thing["moves"]) is not bool or type(thing.get("why", "")) is not str or \
                not isinstance(thing["read"], list) or not isinstance(thing["pointers"], list) or \
                not isinstance(thing.get("pinned", []), list) or \
                any(not isinstance(run, list) or len(run) != 2 or any(type(number) is not int for number in run)
                    for run in thing["read"] + thing.get("pinned", [])) or \
                any(not isinstance(pointer, dict) or set(pointer) - POINTER_FIELDS or
                    any(type(pointer.get(field)) is not int for field in ("low", "high", "value", "at")) or
                    pointer.get("operand", True) is not True or
                    type(pointer.get("named", "")) is not str or ("named" in pointer and "operand" not in pointer) or
                    type(pointer.get("runs", 1)) is not int or pointer.get("runs", 1) < 1
                    for pointer in thing["pointers"]):
            raise PointersError("a thing of the manifest has a field of the wrong type")
        if thing["offset"] < end or thing["size"] < 1 or thing["offset"] + thing["size"] > size or \
                thing["offset"] // BANK != (thing["offset"] + thing["size"] - 1) // BANK:
            raise PointersError("the manifest's things are not inside the ROM, one after another, each in one bank")
        if any(not 0 <= first <= first + count <= thing["size"]
               for first, count in thing["read"] + thing.get("pinned", [])) or \
                any(not (0 <= pointer["low"] < size and 0 <= pointer["high"] < size and
                         0 <= pointer["at"] < thing["size"]) for pointer in thing["pointers"]):
            raise PointersError(f"the thing at {where(thing['offset'])} has a run or a pointer that is not where "
                                f"it can be")
        if thing["moves"] and (not thing["pointers"] or
                               any("operand" in pointer and "named" not in pointer for pointer in thing["pointers"])):
            raise PointersError(f"the thing at {where(thing['offset'])} is said to move, and has no pointer in "
                                f"data to turn, or an operand no source names")
        end = thing["offset"] + thing["size"]
    return things


# ---------------------------------------------------------------------------
# Free space


def taken_bytes(regions: list[dict], size: int, things: list[dict] | None,
                views: list[tuple[int, int, str]]) -> tuple[bytearray, dict[int, str]]:
    """Which bytes of the ROM are not free to be written over, as a number for
    each: 0 is free, and the others are named in the dictionary."""
    names = {1: "code or the header", 2: "bytes the game was seen to read", 3: "something a pointer leads to",
             4: "a pointer", 5: "bytes a picture or a file of text has"}
    taken = bytearray(size)
    for region in regions:
        start = region["offset"]
        if region["kind"] != "data":
            taken[start:start + region["size"]] = b"\x01" * region["size"]
        for seen in region.get("seen", ()):
            taken[start + seen["offset"]:start + seen["offset"] + seen["size"]] = b"\x02" * seen["size"]
    for thing in things or ():
        for first, count in thing["read"]:
            taken[thing["offset"] + first:thing["offset"] + first + count] = b"\x03" * count
        for pointer in thing["pointers"]:
            taken[thing["offset"] + pointer["at"]] = 3
            taken[pointer["low"]] = taken[pointer["high"]] = 4
    for start, end, _ in views:
        taken[start:end] = b"\x05" * (end - start)
    return taken, names


def propose_free(regions: list[dict], image: bytes, things: list[dict] | None,
                 views: list[tuple[int, int, str]]) -> list[tuple[int, int, int]]:
    """The empty end of each bank, as (offset, size, the byte it is full of):
    the run of one byte value that reaches the bank's last byte, in data no
    record read and nothing was drawn from. Its first bytes are left out,
    since whatever lies before it may end in that value too."""
    taken, _ = taken_bytes(regions, len(image), things, views)
    found = []
    for bank_start in range(0, len(image), BANK):
        bank_end = min(bank_start + BANK, len(image))
        value = image[bank_end - 1]
        start = bank_end
        while start > bank_start and image[start - 1] == value and not taken[start - 1]:
            start -= 1
        if start > bank_start:
            start += FREE_MARGIN
        if bank_end - start >= FREE_LEAST:
            found.append((start, bank_end - start, value))
    return found


def free_document(runs: list[tuple[int, int, int]]) -> dict:
    return {"schema": FREE_SCHEMA, "version": 1,
            "free": [{"bank": place(offset)[0], "address": place(offset)[1], "size": size}
                     for offset, size, _ in runs]}


def load_free(path: Path, regions: list[dict], size: int, things: list[dict] | None,
              views: list[tuple[int, int, str]]) -> list[tuple[int, int]] | None:
    """The free space a tree declares, as (offset, size), or None where it
    declares none. Nothing is written outside it, so it is checked: each run
    in one bank, all of it data, none of it read by the game in any record,
    led to by a pointer, or drawn as a picture or text."""
    if not Path(path).exists():
        return None
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise PointersError(f"cannot read {path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema") != FREE_SCHEMA or document.get("version") != 1 or \
            not isinstance(document.get("free"), list):
        raise PointersError(f"{path} is not a version 1 {FREE_SCHEMA} file")
    taken, names = taken_bytes(regions, size, things, views)
    starts = [thing["offset"] for thing in things or ()]
    runs = []
    for entry in document["free"]:
        if not isinstance(entry, dict) or set(entry) != {"bank", "address", "size"} or \
                any(type(number) is not int for number in entry.values()):
            raise PointersError(f"{path}: a run of free space is a bank, an address and a size, all numbers")
        bank, address, count = entry["bank"], entry["address"], entry["size"]
        low = 0 if bank == 0 else BANK
        if bank < 0 or count < 1 or not low <= address <= address + count <= low + BANK or \
                bank * BANK + address - low + count > size:
            raise PointersError(f"{path}: {count} bytes at {bank:02X}:{address:04X} are not inside one bank of "
                                f"the ROM")
        offset = bank * BANK + address - low
        for at in range(offset, offset + count):
            if taken[at]:
                raise PointersError(f"{path} declares {count} bytes at {bank:02X}:{address:04X} free, and "
                                    f"{where(at)} is {names[taken[at]]}")
        # A thing runs to the end of its file, and the end of a bank's last file is where free
        # space is: a thing stops where free space starts. So none may lie in front of what was
        # read of one.
        index = bisect.bisect_right(starts, offset) - 1
        if index >= 0 and offset < things[index]["offset"] + things[index]["size"]:
            thing = things[index]
            used = max([first + size for first, size in thing["read"]] + [pointer["at"] + 1 for pointer in
                                                                          thing["pointers"]])
            if offset < thing["offset"] + used:
                raise PointersError(f"{path} declares {count} bytes at {bank:02X}:{address:04X} free, and the game "
                                    f"read on behind them, as far as {where(thing['offset'] + used - 1)}, in what "
                                    f"starts at {where(thing['offset'])}")
        runs.append((offset, count))
    runs.sort()
    for (offset, count), (other, _) in zip(runs, runs[1:]):
        if offset + count > other:
            raise PointersError(f"{path} declares the bytes at {where(other)} free twice")
    return runs


# ---------------------------------------------------------------------------
# Moving


def move(image: bytes, things: list[dict] | None, free: list[tuple[int, int]] | None,
         grown: list[tuple[int, int, bytes, str]], everything: bool) -> tuple[bytes, dict]:
    """The image with things moved into free space and their pointers turned.

    `grown` is the strings that were made longer, each as (ROM offset, the
    size it has there, its new bytes, how to name it in a refusal): the thing
    each is part of is moved, with the string in it as it is now and what
    followed it following it. With `everything`, every thing that can move is
    moved as well, as far as there is room, and the bytes the game was seen
    to read at each old place are each turned into their opposite: whatever
    still read one would read something else.

    Returns the image and what was done: `moved` as (old offset, old size,
    new offset, new size), `turned` as (offset of the pointer's low byte, what
    it said, what it says), `named` as (the name, what it led to, what it
    leads to) for each operand the game's source names, which is not turned
    in the image, `no_room` as (offset, size), and `left`, the free bytes
    that remain in each bank that was used."""
    starts = [thing["offset"] for thing in things or ()]
    chosen: dict[int, list[tuple[int, int, bytes]]] = {}       # a thing's place in `things` -> its grown strings
    blame: dict[int, str] = {}
    def thing_at(offset: int) -> int | None:
        index = bisect.bisect_right(starts, offset) - 1
        return index if index >= 0 and offset < things[index]["offset"] + things[index]["size"] else None

    for offset, size, new, said in grown:
        # A line of text may be several strings that lie one after another, each a thing of its
        # own, and more than one of them may have been changed. So the line is taken as the old
        # bytes with pieces put in, taken out and replaced, and each such piece has to be in one
        # thing. What a line that grew has shorter is moved the same way.
        old = image[offset:offset + size]
        pieces = [(first, stop - first, new[start:end]) for what, first, stop, start, end in
                  difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes() if what != "equal"]
        for first, was, now in pieces:
            at = offset + first
            # Bytes put in exactly where a thing starts are the start of that thing; put in behind
            # the last thing of the line, they are its end.
            index = thing_at(at) if things else None
            if index is None and things and was == 0 and first > 0:
                index = thing_at(at - 1)
            if index is None:
                raise PointersError(said + "no record says what pointers lead to it: record the game reading it "
                                           "with --data-use and --data-use-pointers, and run `pointers` on the "
                                           "record")
            thing = things[index]
            if at + was > thing["offset"] + thing["size"]:
                raise PointersError(said + f"what was changed in it runs over the end of one thing into the "
                                           f"next, at {where(thing['offset'] + thing['size'])}: each has its own "
                                           f"pointers")
            if not thing["moves"]:
                raise PointersError(said + f"the {thing['size']} bytes at {where(thing['offset'])} it is part of "
                                           f"cannot be moved: {thing['why']}")
            chosen.setdefault(index, []).append((at - thing["offset"], was, now))
            blame.setdefault(index, said)
    if everything:
        if things is None:
            raise PointersError("nothing says what can move: record the game with --data-use and "
                                "--data-use-pointers, and run `pointers` on the record")
        for index, thing in enumerate(things):
            if thing["moves"]:
                chosen.setdefault(index, [])
    done = {"moved": [], "turned": [], "named": [], "no_room": [], "left": {}, "filled": 0, "pinned": 0,
            "everything": everything}
    if not chosen:
        return image, done
    first_said = next(iter(blame.values()), "")
    if free is None:
        raise PointersError(first_said + f"no free space is declared for it to move to: `free` proposes the empty "
                                         f"ends of banks, and {FREE} at the top of the tree declares them")
    for index in sorted(chosen):
        for pointer in things[index]["pointers"]:
            says = image[pointer["low"]] | (image[pointer["high"]] << 8)
            if says != pointer["value"]:
                raise PointersError(blame.get(index, "") + f"the pointer at {where(pointer['low'])} said "
                                    f"{pointer['value']:04X} when `pointers` was run and says {says:04X} now: it "
                                    f"has been edited. Run `pointers` again on the records")
    # A pointer's bytes inside a string that is typed again would be whatever was typed.
    for index, strings in chosen.items():
        base = things[index]["offset"]
        for rel, size, _ in strings:
            for other in chosen:
                for pointer in things[other]["pointers"]:
                    if any(base + rel <= pointer[half] < base + rel + size for half in ("low", "high")):
                        raise PointersError(blame[index] + f"the pointer at {where(pointer['low'])} lies inside "
                                            f"it, and would have to be turned")

    def extent(index: int) -> int:
        """How much of a thing is copied: all of it, or as far as free space starts inside it;
        and on behind it as far as the source says what a name stands for runs. A thing ends
        where the records stopped reading or a file does, and a table the game counts along
        goes on past both: what is copied too is what the game would have found there."""
        thing = things[index]
        inside = [start for start, _ in free if thing["offset"] < start < thing["offset"] + thing["size"]]
        size = min(inside, default=thing["offset"] + thing["size"]) - thing["offset"]
        runs = max((pointer["at"] + pointer["runs"] for pointer in thing["pointers"] if "runs" in pointer),
                   default=0)
        bank_end = (thing["offset"] // BANK + 1) * BANK
        return max(size, min(runs, bank_end - thing["offset"]))

    def new_size(index: int) -> int:
        return extent(index) + sum(len(new) - size for _, size, new in chosen[index])

    room: dict[int, list[list[int]]] = {}
    for offset, size in sorted(free):
        room.setdefault(offset // BANK, []).append([offset, offset + size])
    placed: dict[int, int] = {}
    # What grew has to fit. Of the rest the smallest go first, so that as many fit as can.
    for index in sorted(chosen, key=lambda index: (not chosen[index], new_size(index) if not chosen[index] else 0,
                                                   things[index]["offset"])):
        thing = things[index]
        bank, need = thing["offset"] // BANK, new_size(index)
        run = next((run for run in room.get(bank, ()) if run[1] - run[0] >= need), None)
        if run is None:
            if not chosen[index]:
                done["no_room"].append((thing["offset"], extent(index)))
                continue
            largest = max((run[1] - run[0] for run in room.get(bank, ())), default=0)
            raise PointersError(blame[index] + f"there is no room for it: the {extent(index)} bytes at "
                                f"{where(thing['offset'])} it is part of are {need} with it, and the free space "
                                f"declared in bank {bank:02X} has "
                                + (f"{largest} in one piece at most" if bank in room else "none"))
        placed[index] = run[0]
        run[0] += need

    def moved_at(index: int, at: int) -> int:
        """Where a byte of a thing is in its copy: behind what grew in front of it. Bytes put in
        exactly at a place a pointer leads to are what it leads to from now on."""
        return at + sum(len(new) - size for rel, size, new in chosen[index] if rel + size <= at and rel < at)

    work = bytearray(image)
    # The pointers first, where they lie now: one that lies inside a thing that moves is then
    # copied with it, already turned.
    for index, target in sorted(placed.items()):
        for pointer in things[index]["pointers"]:
            address = place(target + moved_at(index, pointer["at"]))[1]
            if "named" in pointer:
                # The source has it by name, and is told where the name leads now.
                done["named"].append((pointer["named"], pointer["value"], address))
                continue
            work[pointer["low"]], work[pointer["high"]] = address & 0xFF, address >> 8
            done["turned"].append((pointer["low"], pointer["value"], address))
    copies = []
    for index, target in sorted(placed.items()):
        thing = things[index]
        base = thing["offset"]
        content = bytearray()
        cursor = 0
        for rel, size, new in sorted(chosen[index]):
            content += work[base + cursor:base + rel] + new
            cursor = rel + size
        content += work[base + cursor:base + extent(index)]
        copies.append((target, bytes(content)))
        done["moved"].append((base, extent(index), target, len(content)))
    for target, content in copies:
        work[target:target + len(content)] = content
    if everything:
        for index in placed:
            base = things[index]["offset"]
            pinned = things[index].get("pinned", ())
            for first, count in things[index]["read"]:
                for at in range(first, first + count):
                    if any(low <= at < low + size for low, size in pinned):
                        done["pinned"] += 1         # in a file of code, which stays as it is
                    else:
                        work[base + at] = image[base + at] ^ 0xFF
                        done["filled"] += 1
    done["left"] = {bank: sum(run[1] - run[0] for run in runs) for bank, runs in sorted(room.items())
                    if any(target // BANK == bank for target in placed.values())}
    return bytes(work), done


def moved_said(done: dict) -> list[str]:
    """What `build` prints of a move."""
    lines = []
    moved, turned = done["moved"], done["turned"]
    if moved:
        longer = sum(new_size - size for _, size, _, new_size in moved)
        lines.append(f"moved {times(len(moved), 'thing')} into free space, {sum(new for _, _, _, new in moved):,} "
                     f"bytes" + (f", {longer:,} more than they were" if longer else "") + f", and turned "
                     f"{times(len(turned), 'pointer')} to {'it' if len(moved) == 1 else 'them'}")
        for old, size, new, new_size in moved[:LISTED]:
            lines.append(f"  {where(old)} ({size} bytes) is at {where(new)}"
                         + (f", and {new_size - size} longer" if new_size != size else ""))
        if len(moved) > LISTED:
            lines.append(f"  and {len(moved) - LISTED} more")
        for low, said, says in turned[:LISTED]:
            lines.append(f"  the pointer at {where(low)} said {said:04X} and says {says:04X}")
        if len(turned) > LISTED:
            lines.append(f"  and {len(turned) - LISTED} more pointers")
        names = sorted(set(done["named"]))
        if names:
            lines.append(f"  {times(len({name for name, _, _ in names}), 'name')} of the game's source "
                         f"{'leads' if len(names) == 1 else 'lead'} somewhere else now: the game has to be built "
                         f"again with the header that says where")
            for name, said, says in names[:LISTED]:
                lines.append(f"  {name} was {said:04X} and is {says:04X}")
            if len(names) > LISTED:
                lines.append(f"  and {len(names) - LISTED} more names")
    if done["everything"] and not moved and not done["no_room"]:
        lines.append("moved nothing: `pointers` found no thing every read of which came through pointers in data")
    if done["filled"]:
        lines.append(f"  the {done['filled']:,} bytes the game was seen to read at the old places are each turned "
                     f"into their opposite: this image is for proving the pointers, not for playing")
    if done["pinned"]:
        lines.append(f"  {done['pinned']:,} more of them lie in files of code, which no image may change, and are "
                     f"as they were")
    if done["no_room"]:
        lines.append(f"  {times(len(done['no_room']), 'thing')} that can move had no room in the free space of "
                     f"{'its' if len(done['no_room']) == 1 else 'their'} bank, "
                     f"{sum(size for _, size in done['no_room']):,} bytes, and stayed")
    for bank, left in done["left"].items():
        lines.append(f"  bank {bank:02X} has {left:,} free bytes left")
    return lines
