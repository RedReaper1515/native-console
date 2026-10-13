#!/usr/bin/env python3
"""Finding the routine that unpacks: `gb_data.py unpackers` is this.

A packed picture is read through one instruction, a byte at a time, and no
byte of it arrives anywhere as itself: the record of a played game
(`--data-use`) says only that the bytes were read. To edit such a picture
without writing a packer for the game, the game's own routine is left to
unpack it and what it made is replaced as it returns. That needs to know
which function the routine is, how it is told where its packed bytes are,
and what it leaves in RAM.

A game run with `--data-use-follow BANK:ADDRESS` as well, the address being
that reading instruction, writes into its record which functions were running
while the instruction read (`followed`). For each of them: how often it was
entered and how, and of the calls in which the instruction read, how many
read one run of bytes whole, from its first byte, with nothing reading on
from its end afterwards. The instruction's own function reads a byte a call,
each joined to the last. The function that unpacks reads a whole run every
call. So do the functions that call it once each, and further out a function
reads several runs. The one to declare is the tightest: the nearest to the
instruction of those that read whole.

For that one, its calls that were kept as samples say the rest:

- where the stream is: a register pair that held the address of the run's
  first byte as the function was entered, in every sample; failing that a
  word of RAM that did;
- its bank: a register that held the bank number in every sample; failing
  that the bank that was mapped as it was entered;
- what it made: the runs of RAM it stored to that are at the same address
  with the same size in every sample. A run only some calls write is not
  output, and neither is one that is small beside the largest: counters and
  flags the routine keeps for itself.

All of it is what the records show and no more. A function nothing calls,
which is only jumped to, is not seen as a function. Two packed pictures that
lie one after the other in the ROM, unpacked one after the other, read as
one run, so a few calls that joined are allowed for (JOINED_SHARE). And what
is made is told from scratch by size alone: read the report.
"""
from __future__ import annotations

import json
from pathlib import Path

SCHEMA = "gbrecomp.unpackers"
USE_SCHEMA = "gbrt.data-use"
BANK = 0x4000
ENTERED = ("call", "jump", "relative", "fall", "interrupt")
ENTERED_SAID = {"call": "by a call", "jump": "by a jump", "relative": "by a relative jump",
                "fall": "by running on into it", "interrupt": "by an interrupt"}
READS = ("whole", "joined", "several", "left", "undecided", "unfinished")
PAIRS = ("hl", "de", "bc")                  # in the order they are preferred
REGISTERS = ("a", "b", "c", "d", "e", "h", "l")
AREAS = {"vram": "video memory", "cart_ram": "cartridge RAM", "wram": "work RAM", "hram": "high RAM"}
JOINED_SHARE = 0.1      # of a function's calls that read one run, how many may have joined another
MADE_LEAST = 16         # a run of fewer bytes is scratch: a tile is 16
MADE_SHARE = 8          # and so is one this many times smaller than the largest
LISTED = 8              # how many runs of a kind the report lists


class UnpackersError(Exception):
    pass


def where(bank: int, address: int) -> str:
    return f"{bank:02X}:{address:04X}"


def times(count: int, one: str, many: str | None = None) -> str:
    return f"{count:,} {one if count == 1 else many or one + 's'}"


def load_followed(paths: list[Path], size: int, checksum: int) -> dict[tuple[int, int], dict]:
    """What the records say of each followed instruction, added up: the
    functions that were running while it read, by where each is entered."""
    readers: dict[tuple[int, int], dict] = {}
    for path in paths:
        try:
            record = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise UnpackersError(f"cannot read the record {path}: {error}") from error
        if not isinstance(record, dict) or record.get("schema") != USE_SCHEMA or record.get("version") != 1:
            raise UnpackersError(f"{path} is not a version 1 {USE_SCHEMA} record")
        if record["rom"]["size"] != size or record["rom"]["global_checksum"] != checksum:
            raise UnpackersError(f"{path} was recorded from another ROM")
        followed = record.get("followed")
        if followed is None:
            raise UnpackersError(f"{path} follows no instruction: record with --data-use-follow BANK:ADDRESS, the "
                                 f"address of the instruction that reads the packed bytes (`uses` lists the "
                                 f"instructions that read)")
        for entry in followed["readers"]:
            reader = readers.setdefault((entry["bank"], entry["address"]), {
                "bytes": 0, "streams": 0, "records": 0, "too_deep": 0, "functions_dropped": 0, "functions": {}})
            reader["bytes"] += entry["bytes"]
            reader["streams"] += entry["streams"]
            reader["records"] += 1
            reader["too_deep"] += followed["too_deep"]
            reader["functions_dropped"] += entry["functions_dropped"]
            for said in entry["functions"]:
                function = reader["functions"].setdefault((said["bank"], said["address"]), {
                    "entered": 0, "by": dict.fromkeys(ENTERED, 0), "read": 0, **dict.fromkeys(READS, 0),
                    "distance": said["distance"], "inside": {}, "samples_dropped": 0, "samples": []})
                function["entered"] += said["entered"]
                for kind in ENTERED:
                    function["by"][kind] += said["by"][kind]
                function["read"] += said["read"]
                for kind in READS:
                    function[kind] += said[kind]
                function["distance"] = min(function["distance"], said["distance"])
                for outer in said["inside"]:
                    key = (outer["bank"], outer["address"])
                    function["inside"][key] = function["inside"].get(key, 0) + outer["times"]
                function["samples_dropped"] += said["samples_dropped"]
                function["samples"] += said["samples"]
    return readers


def reads_whole(function: dict) -> bool:
    """Whether every call of it reads one run whole, as far as the records
    say: none read several or left without returning, and no more than a
    few read a run that joined another."""
    one_run = function["whole"] + function["joined"]
    return function["whole"] > 0 and not function["several"] and not function["left"] and \
        function["joined"] <= JOINED_SHARE * one_run


def said_reads(function: dict) -> str:
    """In a line, what a function's calls read."""
    if not function["read"]:
        return f"{times(function['unfinished'], 'call')} still going when the record ended"
    parts = [f"{function[kind]:,} {kind}" for kind in READS[:-1] if function[kind]]
    if function["unfinished"]:
        parts.append(f"{function['unfinished']:,} still going at the end")
    return ", ".join(parts)


def stream_address(samples: list[dict]) -> tuple[object | None, str]:
    """Where the function is told its stream starts, and what says so."""
    count = len(samples)
    starts = {(sample["streams"][0]["bank"], sample["streams"][0]["address"]) for sample in samples}
    seen = f"in all {times(count, 'sample')}, of {times(len(starts), 'different stream')}"
    pairs = [pair for pair in PAIRS if all(pair in sample["held"]["registers"] for sample in samples)]
    # A word of RAM has to be in every sample's list. A sample that held the address in more words
    # than a record keeps says nothing of a word it does not list, and is left out of that.
    listed = [sample["held"]["ram16"] for sample in samples if not sample["held"]["ram16_dropped"]]
    words = sorted(word for word in {word for sample in samples for word in sample["held"]["ram16"]}
                   if listed and all(word in there for there in listed))
    guess = "" if len(starts) > 1 else ": one stream is no proof, record more"
    if pairs:
        others = [f"register {pair}" for pair in pairs[1:]] + [f"the word at {word:04X}" for word in words]
        also = f" (so did {', '.join(others)})" if others else ""
        return {"register": pairs[0]}, f"register {pairs[0]} held it {seen}{also}{guess}"
    if words:
        also = f" (so did the {'word' if len(words) == 2 else 'words'} at " \
               f"{', '.join(f'{word:04X}' for word in words[1:])})" if len(words) > 1 else ""
        full = len(samples) - len(listed)
        if full:
            also += f" ({times(full, 'sample')} held it in more words than a record keeps, and " \
                    f"{'says' if full == 1 else 'say'} nothing either way)"
        return {"ram16": words[0]}, f"the word of RAM at {words[0]:04X} held it {seen}{also}{guess}"
    if len(starts) == 1:
        address = samples[0]["streams"][0]["address"]
        return address, f"always {address:04X} {seen}: nothing held it, and one stream is no proof"
    return None, f"no register pair and no word of RAM held where the stream starts in every one of " \
                 f"{times(count, 'sample')}"


def stream_bank(samples: list[dict]) -> tuple[object | None, str]:
    """How the function is told the bank its stream is in."""
    count = len(samples)
    banks = {sample["streams"][0]["bank"] for sample in samples}
    seen = f"in all {times(count, 'sample')}, of {times(len(banks), 'different bank')}"
    if all(sample["streams"][0]["address"] < BANK and sample["streams"][0]["bank"] == 0 for sample in samples):
        return 0, "every stream seen is below 4000, where the bank is always 0"
    registers = [name for name in REGISTERS if all(name in sample["bank_in"] for sample in samples)]
    mapped = all(sample["mapped_bank"] == sample["streams"][0]["bank"] for sample in samples)
    guess = "" if len(banks) > 1 else ": one bank is no proof, record more"
    if registers:
        others = [f"register {name}" for name in registers[1:]] + (["the bank mapped as it is entered"] if mapped
                                                                   else [])
        also = f" (so did {', '.join(others)})" if others else ""
        return {"register": registers[0]}, f"register {registers[0]} held it {seen}{also}{guess}"
    if mapped:
        return {"mapped_bank": True}, f"the bank mapped as it is entered was it {seen}{guess}"
    if len(banks) == 1:
        bank = samples[0]["streams"][0]["bank"]
        return bank, f"always {bank} {seen}: no register held it and it was not the bank mapped, and one bank " \
                     f"is no proof"
    return None, f"no register held the bank of the stream in every one of {times(count, 'sample')}, and it was " \
                 f"not always the bank mapped"


def run_said(run: tuple) -> str:
    area, bank, address, size = run
    banked = f", bank {bank}" if bank else ""
    return f"{address:04X}, {times(size, 'byte')} ({AREAS.get(area, area)}{banked})"


def made_runs(samples: list[dict]) -> tuple[list[dict], list[str]]:
    """What the function leaves in RAM: the runs it stored to that are the
    same in every sample, less what is too small to be output. And lines
    saying what was not taken, and why."""
    count = len(samples)
    seen: dict[tuple, int] = {}                     # (area, bank, address, size) -> samples it is in
    starts: dict[tuple, set[int]] = {}              # (area, bank, address) -> the sizes it had
    for sample in samples:
        for run in sample["wrote"]:
            key = (run["area"], run["bank"], run["address"], run["size"])
            seen[key] = seen.get(key, 0) + 1
            starts.setdefault(key[:3], set()).add(run["size"])
    every = sorted((key for key, there in seen.items() if there == count), key=lambda key: key[2])
    largest = max((key[3] for key in every), default=0)
    made, notes = [], []
    small = []
    for key in every:
        if key[3] >= MADE_LEAST and key[3] * MADE_SHARE >= largest:
            made.append({"address": key[2], "size": key[3], "_said": f"{run_said(key)}, in all "
                                                                    f"{times(count, 'sample')}"})
        else:
            small.append(key)
    # A buffer the caller names: a run that starts where a register pair pointed, the same size every time.
    for pair in PAIRS:
        sizes = set()
        places = set()
        for sample in samples:
            at = sample["registers"][pair]
            there = [run for run in sample["wrote"] if run["address"] == at]
            sizes.add(there[0]["size"] if there else None)
            places.add(at)
        size = next(iter(sizes))
        if len(sizes) == 1 and size is not None and len(places) > 1 and size >= MADE_LEAST and \
                size * MADE_SHARE >= largest:
            made.append({"address": {"register": pair}, "size": size,
                         "_said": f"{times(size, 'byte')} from where register {pair} pointed, in all "
                                  f"{times(count, 'sample')}, at {times(len(places), 'different address', 'different addresses')}"})
    for key in sorted(small, key=lambda key: -key[3])[:LISTED]:
        beside = f"small beside {largest:,}" if key[3] * MADE_SHARE < largest else f"fewer than {MADE_LEAST}"
        notes.append(f"{run_said(key)}, in all {times(count, 'sample')}: {beside}, so scratch")
    if len(small) > LISTED:
        notes.append(f"and {times(len(small) - LISTED, 'smaller run')} like those")
    # The same place with another size each time: output perhaps, of no one size.
    varying = sorted((start for start, sizes in starts.items()
                      if len(sizes) > 1 and sum(there for key, there in seen.items() if key[:3] == start) == count),
                     key=lambda start: start[2])
    for area, bank, address in varying[:LISTED]:
        sizes = starts[(area, bank, address)]
        notes.append(f"{address:04X} ({AREAS.get(area, area)}), in all {times(count, 'sample')} but of "
                     f"{min(sizes):,} to {max(sizes):,} bytes: a declaration needs one size")
    once = sorted((key for key, there in seen.items() if there < count and key[:3] not in varying),
                  key=lambda key: (-key[3], key[2]))
    for key in once[:LISTED]:
        notes.append(f"{run_said(key)}, in {seen[key]:,} of {times(count, 'sample')}: not every call writes it")
    if len(once) > LISTED:
        notes.append(f"and {times(len(once) - LISTED, 'smaller run')} that not every call writes")
    dropped = sum(sample["wrote_dropped"]["runs"] for sample in samples)
    if dropped:
        notes.append(f"and {times(dropped, 'run')} the record had no room for, the smallest of their samples")
    return made, notes


def propose(reader: tuple[int, int], place: tuple[int, int], function: dict) -> tuple[dict | None, list[str]]:
    """The declaration of one function as the routine that unpacks, or why
    there is none; and the lines of the report either way."""
    samples = [sample for sample in function["samples"] if sample["read"] == "whole"]
    lines = []
    one_run = function["whole"] + function["joined"]
    calls = f"{function['whole']:,} of its {times(one_run, 'call')} that read one run read it whole"
    if function["joined"]:
        calls += f" ({function['joined']:,} joined another run: streams that lie one after the other in the ROM " \
                 f"read as one)"
    if function["undecided"]:
        calls += f"; {function['undecided']:,} more had read one when their record ended"
    lines.append(calls)
    entered = ", ".join(f"{function['by'][kind]:,} {ENTERED_SAID[kind]}" for kind in ENTERED if function["by"][kind])
    lines.append(f"entered {times(function['entered'], 'time')}: {entered}")
    unseen = function["by"]["relative"] + function["by"]["fall"]
    if unseen:
        lines.append(f"  mind: {unseen:,} of those {'was' if unseen == 1 else 'were'} by a relative jump or by "
                     f"running on into it, which is neither a call nor a JP: what watches calls and jumps does "
                     f"not see it entered then")
    if not samples:
        lines.append("no call of it that read a run whole was kept as a sample: nothing to work out the rest from")
        return None, lines
    address, said = stream_address(samples)
    lines.append(f"stream address: {said}")
    bank, said_bank = stream_bank(samples)
    lines.append(f"stream bank: {said_bank}")
    made, notes = made_runs(samples)
    for run in made:
        lines.append(f"made: {run['_said']}")
    if not made:
        lines.append(f"made: nothing it stored to RAM is the same in all {times(len(samples), 'sample')} and of "
                     f"a size to be output")
    if notes:
        lines.append("not taken as made:")
        lines += [f"  {note}" for note in notes]
    if address is None or bank is None or not made:
        return None, lines
    records = "the record" if len(samples) == 1 else "the records"
    why = f"of the functions running while {where(*reader)} read, the nearest to it whose calls each read one " \
          f"run of bytes whole: {function['whole']:,} of {one_run:,} did, and {times(len(samples), 'sample')} in " \
          f"{records} agree on where the stream is and on what is made"
    declaration = {"name": f"unpacker_{place[0]:02X}_{place[1]:04X}",
                   "function": {"bank": place[0], "address": place[1]},
                   "stream": {"bank": bank, "address": address},
                   "made": [{"address": run["address"], "size": run["size"]} for run in made],
                   "why": why}
    return declaration, lines


def chain(reader: dict) -> list[tuple[tuple[int, int], dict]]:
    """The functions outward from the instruction: nearest first."""
    return sorted(reader["functions"].items(), key=lambda item: (item[1]["distance"], item[0]))


def report(place: tuple[int, int], reader: dict, regions: list[dict] | None = None) -> tuple[list[dict], list[str]]:
    """What one followed instruction's records say: the lines to print, and
    the declarations proposed."""
    lines = [f"followed {where(*place)}: it read {times(reader['bytes'], 'byte')} in "
             f"{times(reader['streams'], 'run')}, in {times(reader['records'], 'record')}"]
    functions = chain(reader)
    if not reader["bytes"]:
        lines.append("  so nothing can be said: is it the instruction that reads? `uses` lists the ones that do, "
                     "as BANK:ADDRESS")
        return [], lines
    if not functions:
        lines.append("  no function was running while it read: nothing called anything before it did")
        return [], lines
    lines.append("  the functions running while it read, nearest first: how many calls out, where it is entered, "
                 "how often it was, and what its calls read")
    for function_place, function in functions:
        inside = ", ".join(f"{where(*outer)} x{count:,}" for outer, count in
                           sorted(function["inside"].items(), key=lambda item: -item[1])[:3])
        mark = "   <- reads whole" if reads_whole(function) else ""
        lines.append(f"    {function['distance']:3d}  {where(*function_place)}  entered {function['entered']:,}: "
                     f"{said_reads(function)}{mark}" + (f"   [in {inside}]" if inside else ""))
    if reader["functions_dropped"] or reader["too_deep"]:
        lines.append(f"  the record left out {times(reader['functions_dropped'], 'call')} for want of room for "
                     f"their functions, and did not follow {times(reader['too_deep'], 'call')} made more than "
                     f"256 deep")
    whole = [(function_place, function) for function_place, function in functions if reads_whole(function)]
    if not whole:
        lines.append("  no function reads one run whole in every call: nothing to propose. This is what an "
                     "instruction looks like that reads a stream which goes on, as music does, or when the "
                     "record has too little of it")
        return [], lines
    proposed: list[dict] = []
    for distance in sorted({function["distance"] for _, function in whole}):
        for function_place, function in whole:
            if function["distance"] != distance:
                continue
            declaration, said = propose(place, function_place, function)
            lines.append(f"  {where(*function_place)}, {'proposed' if declaration else 'not proposed'}:")
            lines += [f"    {line}" for line in said]
            if declaration:
                nearest_place, nearest = whole[0]
                if function["read"] < nearest["read"]:
                    lines.append(f"    mind: {where(*nearest_place)}, nearer, read in {times(nearest['read'], 'call')} "
                                 f"and this in {function['read']:,}: it is one way to the routine among others, "
                                 f"and declares only the streams that come through it")
                if regions is not None:
                    lines.append(f"    {streams_said(function, regions)}")
                proposed.append(declaration)
        if proposed:
            break
        lines.append("  so the next further out:")
    if not proposed:
        lines.append("  nothing to propose: of no function that reads whole can the records say where its stream "
                     "is and what it makes")
    return proposed, lines


def streams_said(function: dict, regions: list[dict]) -> str:
    """Where in the data tree the streams of a function's samples lie."""
    files: dict[str, int] = {}
    in_code = 0
    streams = set()
    for sample in function["samples"]:
        if sample["read"] != "whole":
            continue
        stream = sample["streams"][0]
        if (stream["offset"], stream["size"]) in streams:
            continue
        streams.add((stream["offset"], stream["size"]))
        for region in regions:
            start = max(stream["offset"], region["offset"])
            stop = min(stream["offset"] + stream["size"], region["offset"] + region["size"])
            if start < stop:
                files[region["file"]] = files.get(region["file"], 0) + stop - start
                if region["kind"] != "data":
                    in_code += stop - start
    total = sum(size for _, size in streams)
    said = f"the {times(len(streams), 'stream')} of its samples, {times(total, 'byte')}, lie in " \
           f"{times(len(files), 'file')} of the tree"
    if in_code:
        said += f"; {times(in_code, 'byte')} of them in files of code, where the analysis decoded instructions"
    return said


def run(records: list[Path], size: int, checksum: int, output: Path | None,
        regions: list[dict] | None = None) -> int:
    readers = load_followed(records, size, checksum)
    if not readers:
        raise UnpackersError("the records follow no instruction")
    declarations: list[dict] = []
    for place, reader in sorted(readers.items()):
        proposed, lines = report(place, reader, regions)
        print("\n".join(lines))
        declarations += proposed
    if not declarations:
        return 1
    document = {"schema": SCHEMA, "version": 1, "unpackers": declarations}
    if output is None:
        print("the declaration (-o FILE writes it):")
        print(json.dumps(document, indent=1))
        return 0
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8", newline="\n")
    except OSError as error:
        raise UnpackersError(f"cannot write {output}: {error}") from error
    print(f"wrote {output}: {times(len(declarations), 'unpacker')} declared. It is a proposal: read it against "
          f"the report before a game is given it")
    return 0
