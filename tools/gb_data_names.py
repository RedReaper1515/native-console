#!/usr/bin/env python3
"""Names for the files of a data tree.

`gb_data.py` uses this for its `names` command, and every time it writes a
tree's manifest.

`split` calls a file by where its bytes lie in the ROM, which is all it
knows. `names.sym`, at the top of the tree, says what to call things
instead. It is an RGBDS symbol file, a name to a line, and anybody can
write it:

    05:4180 RedSprite
    0B:4D7D PikachuPicFront ; from game.sym

A line names whatever the tree has at that address: the data region that
starts there, the picture of tiles that starts there, the string that
starts there, the things a routine unpacked from the stream there. The
first line for an address is the one that counts. A line for RAM, a
constant, or an address the tree has nothing at is left where it is and
names nothing yet. A file of code, or the header, keeps its name: that it
is code, and not to be edited, is what its name is there to say.

`names DIR --sym FILE` brings names in from a symbol file somebody has: a
label of it gets a line, marked `; from FILE`, if it lies in a data region
(at its start, or further in, where a picture or a string starts or may be
found to start later) or on an unpacked stream. Where a symbol file has
several labels for one address they all get a line, a label of its own
before one inside another (`Routine.loop`), and last a label that says
where another ends (`FontEnd`, where there is a `Font`). Bringing in only
ever adds lines. An address that has a line keeps it, so a line somebody
wrote or changed stays as it is; with the line deleted, the symbol file's
name comes back the next time.

A file with a name is called by it, in the folder it was in:

    bank_05/4180_unknown.bin               bank_05/RedSprite.bin
    bank_05/4180_unknown.tiles.png         bank_05/RedSprite.tiles.png
    bank_05/4180_unknown.text.txt          bank_05/RedSprite.text.txt
    bank_05/4146_unknown.tiles_4180.png    bank_05/RedSprite.tiles.png
    bank_05/4180_unknown.bin               bank_05/RedSprite_and_2_more.bin
    unpacked/pictures/0B_4D7D.png          unpacked/pictures/PikachuPicFront.png
    unpacked/pictures/0B_4D7D_2.made       unpacked/pictures/PikachuPicFront_2.made

A region is a run of bytes between code and may hold more than the thing
its first byte starts. Where other places in it have a name too, its file
is called by the first and how many others there are, as in the fifth row
(the same region, with two more sprites in it): by one name alone it would
pass for that one thing. The count is of what
`labels` lists, a place counted once however many labels it has. A label
that says where another ends counts where it has its place to itself,
since something lies after that end, and is not the name where it shares
its place with another. A region with no name on its first byte keeps its
address, whatever lies further in.

A view is one thing, and goes by the name of where it starts if that has
one, at its region's first byte too; otherwise by its region's file, count
and all. The ways a stream was unpacked share the stream's name. A tree
named by an earlier rule is renamed by this one the next time `names`, or
any command that writes the manifest, is run on it.
What a file system does not take in a name is written `_`. Two things that
would get the same file name (the same label twice, or two that differ only
in capitals) both keep their old name after the label,
`Label.4180_unknown.bin`, and so does a label that is a device's name, is
longer than 80 characters, or looks like a name of this tool's own. Where a
file the manifest does not know is in the way, and is not the same bytes,
the thing goes without its name and the tool says so. A file whose name
this tool neither gave nor has a record of is somebody's, and is left.

The address is not lost: the manifest has it for every file as before, and
under `names` it has what each renamed file was called, which is the name
the file gets back when its line is taken away. A string has no file of its
own, so its name is a comment over its line in the file of text, `# BB:AAAA
Label`, or `Label+N` for a string that starts N bytes after the nearest
name before it in its region. And since a region is not cut up, each region
lists the names that lie in it (`labels`), for whoever draws a view there.

Nothing in a file is changed by any of this but those comments: a file
keeps its edit through a rename, and a tree builds to the same image and
the same pack named and unnamed. Files are renamed all or none, and a
renaming that is cut short is taken back by the next command (`recover`).

`apply` is what does it, and `gb_data.py` calls it from `write_manifest`:
whatever command made a file, it is called by its name before the manifest
says so. A command that has to find the file a view already has asks
`kept`. A tree with no names.sym is not touched.

`names DIR --seen` also calls a data file that has no name by what the game
was seen to use most of its bytes as (`uses`): `4146_tiles.bin` for
`4146_unknown.bin`. The address stays in front, so such a name is not taken
for one somebody gave.
"""
from __future__ import annotations

from bisect import bisect_right
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys

NAMES = "names.sym"
JOURNAL = "names.renaming"          # there only while files are being renamed
BANK = 0x4000
LONGEST = 80                        # a label of more characters is cut, and keeps its old name after it
ADDRESS = re.compile(r"([0-9A-Fa-f]{2,4}):([0-9A-Fa-f]{4})")
MARK = re.compile(r"\s*from\s+(\S+)")
END = re.compile(r"(.+?)[._]?[Ee]nd")
# What no file system takes in a name, or a shell splits one at.
UNWRITABLE = re.compile(r'[<>:"/\\|?*\x00-\x20\x7f]')
DEVICES = frozenset(["con", "prn", "aux", "nul"] +
                    [f"{port}{number}" for port in ("com", "lpt") for number in "123456789"])
# A file with no name is called `4180_unknown` or `0B_4D7D_2`: a label like that is not used by itself.
OWN = re.compile(r"[0-9A-Fa-f]{4}_[a-z_]+|[0-9A-Fa-f]{2}_[0-9A-Fa-f]{4}(_\d+)?")
# How a view's file name ends: what it is a view as, the address it starts at if that is not
# where its region starts, and the kind of file.
VIEW_END = re.compile(r"(\.[A-Za-z0-9_]+?)(_[0-9A-Fa-f]{4})?(\.[A-Za-z0-9]+)$")
# The comment that names a string, a string's line, and the line of a file of text that has its region's file in it.
STRING_NAME = re.compile(r"# [0-9A-F]{2,4}:[0-9A-F]{4} \S+")
STRING_LINE = re.compile(r"\+([0-9A-Fa-f]+) \d+ \|")
TEXT_HEAD = re.compile(r"(# The strings of )(\S+)(, one to a line)")
USES = ("tiles", "tile_map", "oam", "sound", "palette", "bank", "jump", "pointer")
HEAD = """\
; Names for the files of this data tree, in the form of an RGBDS symbol file: BB:AAAA Label.
; A line names what the tree has at that address: a region, a picture of tiles, a string, or
; what was unpacked from the stream there. The first line for an address counts.
; Write, change and delete lines as you like, then run `gb_data.py names` on the tree.
; A line marked `; from FILE` was brought in from that symbol file. Bringing in only adds lines.
"""


class NamesError(Exception):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def home(bank: int, address: int) -> tuple[int, int] | None:
    """An address as the tree has it: the home bank is bank 0 whatever bank is written in front
    of it, and nothing from 8000 up is in the ROM."""
    if address < BANK:
        return 0, address
    return (bank, address) if address < 2 * BANK else None


def rom_offset(where: tuple[int, int]) -> int:
    return where[1] if where[1] < BANK else where[0] * BANK + where[1] - BANK


def on_disk(directory: Path, name: str) -> Path:
    return directory.joinpath(*PurePosixPath(name).parts)


def safe(name) -> bool:
    """Whether a name is a path inside the tree, written as the manifest writes one."""
    if not isinstance(name, str):
        return False
    parts = PurePosixPath(name).parts
    return bool(parts) and ".." not in parts and not name.startswith("/") and ":" not in name and "\\" not in name


def check(manifest: dict) -> None:
    """What the manifest says of names has to be what this tool wrote: files are renamed by it."""
    section = manifest.get("names", {})
    if not isinstance(section, dict) or set(section) - {"was", "seen"} or \
            not isinstance(section.get("was", {}), dict) or section.get("seen", True) is not True or \
            any(not safe(now) or not safe(before) for now, before in section.get("was", {}).items()):
        raise NamesError("the manifest's `names` is not what `gb_data.py names` writes")
    for region in manifest["regions"]:
        labels = region.get("labels", [])
        if not isinstance(labels, list) or any(
                not isinstance(label, list) or len(label) != 2 or type(label[0]) is not int or
                type(label[1]) is not str for label in labels):
            raise NamesError(f"the `labels` of {region.get('file')} are not a list of offsets and names")


def line_address(line: str) -> tuple[list[str], tuple[int, int] | None, str]:
    """A line of a symbol file: its words, the ROM address it names if it names one, and its comment."""
    body, _, comment = line.partition(";")
    words = body.split()
    place = ADDRESS.fullmatch(words[0]) if len(words) == 2 else None
    return words, home(int(place.group(1), 16), int(place.group(2), 16)) if place else None, comment


def read_names(directory: Path) -> tuple[list[str], str, dict[tuple[int, int], tuple[str, str]], list[str]]:
    """What names.sym says: its lines as they are, how it ends a line, the name of each ROM
    address with the symbol file it was brought in from (or nothing), and a remark for each
    line that cannot be read. A line that cannot be read names nothing, and stops nothing."""
    try:
        raw = (directory / NAMES).read_bytes()
    except FileNotFoundError:
        return [], "\n", {}, []
    except OSError as error:
        return [], "\n", {}, [f"cannot read {directory / NAMES}: {error}"]
    lines = raw.decode("utf-8-sig", errors="replace").splitlines()
    names: dict[tuple[int, int], tuple[str, str]] = {}
    remarks = []
    for number, line in enumerate(lines, 1):
        words, where, comment = line_address(line)
        if where is not None and where not in names:
            mark = MARK.match(comment)
            names[where] = (words[1], mark.group(1) if mark else "")
        # RAM has names too, and a constant, as rgblink writes one, is a number and a name.
        elif words and not (len(words) == 2 and re.fullmatch(r"[0-9A-Fa-f]+(:[0-9A-Fa-f]{4})?", words[0])):
            remarks.append(f"{NAMES}, line {number} is not 'BB:AAAA Label', and names nothing")
    return lines, "\r\n" if b"\r\n" in raw else "\n", names, remarks


def tree_places(manifest: dict) -> tuple[list[int], set[tuple[int, int] | None]]:
    """Where each region starts in the ROM, in order, and the streams things were unpacked from."""
    starts = [region["offset"] for region in manifest["regions"]]
    return starts, {home(thing["bank"], thing["address"]) for thing in manifest.get("unpacked", ())}


def bring_in(directory: Path, manifest: dict, sym_files: list[Path], seen: bool | None = None) -> list[str]:
    """Add to names.sym the labels of symbol files that lie on what the tree has, and make the
    file if it is not there. Returns what to say about it. An address that has a line already
    keeps it, whoever wrote it. `seen` turns naming by what the game was seen to do on or off."""
    import gb_lift_score        # its reader of RGBDS symbol files: only this needs it

    if seen is not None:
        manifest["names"] = dict({"seen": True} if seen else {}, was=manifest.get("names", {}).get("was", {}))
    lines, newline, names, _ = read_names(directory)
    starts, streams = tree_places(manifest)
    regions = manifest["regions"]
    size = manifest["rom"]["size"]
    new: dict[tuple[int, int], tuple[list[str], str]] = {}
    said = []
    for path in sym_files:
        try:
            labels = gb_lift_score.load_sym(path)
        except OSError as error:
            raise NamesError(f"cannot read the symbol file {path}: {error}") from error
        if not labels:
            raise NamesError(f"{path} has no line like 'BB:AAAA Label' for an address in the ROM: it is not an "
                             f"RGBDS symbol file of a game")
        source = UNWRITABLE.sub("_", Path(path).name)
        every = {label for found in labels.values() for label in found}
        added = there = differ = elsewhere = 0
        for (bank, address), found in sorted(labels.items()):
            where = home(bank, address)
            offset = rom_offset(where)
            region = regions[bisect_right(starts, offset) - 1] if offset < size else None
            if region is None or (region["kind"] != "data" and where not in streams):
                elsewhere += 1          # in code: a file of code keeps its name, and has no views
            elif where in names or where in new:    # a line of the file's, or of a symbol file given before this one
                there += 1
                differ += not set(found) & set([names[where][0]] if where in names else new[where][0])
            else:
                # The first line is the one that names: a label of its own before one inside
                # another (`Routine.loop`), and last a label that marks where another ends.
                new[where] = (sorted(found, key=lambda label: (marks_end(label, every), "." in label)), source)
                added += 1
        said.append(f"{path}: {added:,} {'address' if added == 1 else 'addresses'} of what the tree has named "
                    f"from it" + (f", {there:,} had a line already and keep it" if there else "")
                    + (f" ({differ:,} of those say something else than the symbol file does)" if differ else "")
                    + f"; {elsewhere:,} of its labels are inside code or not in this ROM")
    if not lines:
        lines = HEAD.splitlines()
    # A new line goes in front of the first line there is for an address after its own, so that
    # a file in order stays in order whatever somebody has added at its end; with no such line,
    # it goes after the last line that names an address.
    placed = sorted((where, index) for index, line in enumerate(lines)
                    for where in [line_address(line)[1]] if where is not None)
    first = [len(lines)] * (len(placed) + 1)        # for each of them, the first line of an address not before it
    for at in range(len(placed) - 1, -1, -1):
        first[at] = min(placed[at][1], first[at + 1])
    end = max((index for _, index in placed), default=len(lines) - 1) + 1
    before: dict[int, list[str]] = {}
    for where, (found, source) in sorted(new.items()):
        at = first[bisect_right(placed, (where, len(lines)))]
        before.setdefault(at if at < len(lines) else end, []).extend(
            f"{where[0]:02X}:{where[1]:04X} {label} ; from {source}" for label in found)
    out = []
    for index, line in enumerate(lines):
        out.extend(before.get(index, []))
        out.append(line)
    out.extend(before.get(len(lines), []))
    if out != lines or not (directory / NAMES).exists():
        try:
            fresh = directory / (NAMES + ".new")
            fresh.write_bytes((newline.join(out) + newline).encode("utf-8"))
            fresh.replace(directory / NAMES)
        except OSError as error:
            raise NamesError(f"cannot write {directory / NAMES}: {error}") from error
    return said


def marks_end(label: str, every: set[str]) -> bool:
    """Whether a label is another label of the same file with `End` after it (`FontEnd` where
    there is a `Font`): it names where that one stops, not what starts there."""
    end = END.fullmatch(label)
    return end is not None and end.group(1) in every


def file_stem(label: str) -> str:
    """A label as the start of a file name: what no file system takes is written `_`."""
    stem = UNWRITABLE.sub("_", label)[:LONGEST].rstrip(". ")
    return "_" + stem[1:] if stem.startswith(".") else stem


def alone(label: str, stem: str) -> bool:
    """Whether a label's file name can do without the old name after it."""
    return len(label) <= LONGEST and stem.split(".")[0].lower() not in DEVICES and not OWN.fullmatch(stem)


def kept(region: dict, kind: str, offset: int | None, made: str) -> str:
    """The file a view of a region has, whatever it has been renamed to, or `made`: the name a
    command makes for a new one. A view is known by its kind and where it starts."""
    for view in region.get("views", ()):
        if view.get("kind") == kind and (offset is None or view.get("offset") == offset):
            return view["file"]
    return made


def seen_as(region: dict) -> str | None:
    """What the game was seen to use most bytes of a region as, as they are."""
    counts = dict.fromkeys(USES, 0)
    for run in region.get("seen", ()):
        for use in run.get("as_it_is", ()):
            if use in counts:
                counts[use] += run["size"]
    most = max(USES, key=lambda use: counts[use])
    return most if counts[most] else None


def folder_and_file(name: str) -> tuple[str, str]:
    """A file's folder, with its slash, and the rest."""
    folder, slash, base = name.rpartition("/")
    return folder + slash, base


def units_of(manifest: dict, names: dict[tuple[int, int], tuple[str, str]], inside: list[list[list]]) -> list[dict]:
    """Everything that is renamed as one: a region with the views that go by its name, a view
    that has a name of its own, and the things unpacked from one stream. `inside` is, for each
    region, the names that lie in it.

    `choices` is what a unit's files could be called, best first: by its label, by its label
    with the old name after it, by its old name (`default` is which that is), and as they are
    now. `at` is which it gets. A file whose name is neither one this tool gives nor one the
    manifest has a record of is somebody's, and is only ever called what it is called now."""
    section = manifest.get("names", {})
    was = section.get("was", {})
    units = []
    for region, labels in zip(manifest["regions"], inside):
        now = region["file"]
        folder, base = folder_and_file(now)
        # What kind of file it is follows the last dot of its name. The name may have dots of its own: a label
        # of a disassembly's local kind (`Menu.yes`) is one, and cut at its first dot two such regions would
        # be the same name, and their views the same file.
        old, _, kind = folder_and_file(was.get(now, now))[1].rpartition(".")
        if not old:
            old, kind = kind, ""
        kind = "." + kind if kind else ""
        current = base[:-len(kind)] if kind and base.endswith(kind) else base
        known = now in was or OWN.fullmatch(old) is not None
        # A file of code, or the header, keeps its name: that it is code is what there is to know of it.
        label = names.get((region["bank"], region["address"]), ("", ""))[0] if region["kind"] == "data" else ""
        stem = file_stem(label)
        # A region is a run of bytes between code, and may hold more than what its first byte is
        # the start of. Called by that one name it would pass for that one thing, so its file
        # says how many other places in it have a name: the count is of what `labels` lists.
        if stem and len(labels) > 1:
            stem = f"{stem}_and_{len(labels) - 1}_more"
        choices = []
        if known and stem:
            choices = ([stem] if alone(label, stem) else []) + [f"{stem}.{old}"]
        elif known and section.get("seen") and region["kind"] == "data" and seen_as(region):
            choices = [f"{region['address']:04X}_{seen_as(region)}"]
        unit = {"what": "region", "region": region, "folder": folder, "kind": kind, "old": old, "known": known,
                "by_label": bool(known and stem), "default": len(choices) if known else 0, "at": 0,
                "choices": choices + ([old] if known else []) + [current], "follows": []}
        units.append(unit)
        for view in region.get("views", ()):
            if not isinstance(view, dict) or not isinstance(view.get("file"), str):
                continue
            end = VIEW_END.search(folder_and_file(view["file"])[1])
            if end is None:
                continue                    # not a name this tool gives a view: left as it is
            as_what, address, kind_of_file = end.group(1), end.group(2) or "", end.group(3)
            offset = view.get("offset") if type(view.get("offset")) is int else None
            if offset is not None:
                address = f"_{region['address'] + offset:04X}" if offset else ""
            # A view is one thing: it goes by the name of where it starts, at its region's first byte too.
            own = names.get((region["bank"], region["address"] + offset), ("", ""))[0] if offset is not None else ""
            own_stem = file_stem(own)
            if not own_stem:
                unit["follows"].append((view, as_what + address + kind_of_file))
                continue
            choices = ([own_stem + as_what + kind_of_file] if alone(own, own_stem) else []) + \
                [f"{own_stem}.{old}{as_what}{address}{kind_of_file}"]
            units.append({"what": "view", "view": view, "folder": folder, "region": unit, "by_label": True,
                          "ending": as_what + address + kind_of_file, "default": len(choices), "at": 0,
                          "choices": choices + [None, folder_and_file(view["file"])[1]]})
    streams: dict[tuple, dict] = {}
    for thing in manifest.get("unpacked", ()):
        key = (thing["unpacker"], thing["bank"], thing["address"])
        unit = streams.get(key)
        if unit is None:
            where = home(thing["bank"], thing["address"])
            unit = streams[key] = {"what": "stream", "things": [], "at": 0, "known": True,
                                   "label": names.get(where, ("", ""))[0] if where else ""}
            units.append(unit)
        now = thing["made"]
        folder, base = folder_and_file(now)
        old = folder_and_file(was.get(now, now))[1]
        old = old[:-len(".made")] if old.endswith(".made") else old
        first = f"{thing['bank']:02X}_{thing['address']:04X}"
        unit["known"] = unit["known"] and (now in was or OWN.fullmatch(old) is not None)
        unit["things"].append({"thing": thing, "folder": folder, "old": old,
                               "current": base[:-len(".made")] if base.endswith(".made") else base,
                               "tail": old[len(first):] if old.startswith(first) else None})
    for unit in streams.values():
        stem = file_stem(unit["label"])
        things = unit["things"]
        choices = []
        if unit["known"] and stem:
            if alone(unit["label"], stem) and all(entry["tail"] is not None for entry in things):
                choices.append([stem + entry["tail"] for entry in things])
            choices.append([f"{stem}.{entry['old']}" for entry in things])
        unit["by_label"] = bool(choices)
        unit["default"] = len(choices) if unit["known"] else 0
        unit["choices"] = choices + ([[entry["old"] for entry in things]] if unit["known"] else []) + \
            [[entry["current"] for entry in things]]
    return units


def wanted(unit: dict) -> list[tuple[dict, str, str]]:
    """The files of a unit as (what holds the name, under which field, the name it is to have)."""
    choice = unit["choices"][unit["at"]]
    if unit["what"] == "region":
        unit["stem"] = choice
        return [(unit["region"], "file", unit["folder"] + choice + unit["kind"])] + \
               [(view, "file", unit["folder"] + choice + ending) for view, ending in unit["follows"]]
    if unit["what"] == "view":              # with no name of its own after all, by its region's
        return [(unit["view"], "file", unit["folder"] + (choice or unit["region"]["stem"] + unit["ending"]))]
    files = []
    for entry, stem in zip(unit["things"], choice):
        thing = entry["thing"]
        files.append((thing, "made", f"{entry['folder']}{stem}.made"))
        shown = entry["folder"] + entry["current"]
        if thing["file"].startswith(shown + "."):           # the file to edit goes by the same name
            files.append((thing, "file", entry["folder"] + stem + thing["file"][len(shown):]))
    return files


def settle(directory: Path, units: list[dict], remarks: list[str]) -> list[tuple[dict, str, str]]:
    """Which name each unit gets, so that no two files get the same one and none lands on a
    file the manifest does not know. Returns every file with the name it is to have."""
    order = sorted(units, key=lambda unit: unit["what"] != "region")        # a view asks its region
    looked: dict[tuple[str, str], bool] = {}
    while True:
        files = [(unit, entry) for unit in order for entry in wanted(unit)]
        ours = {holder[field].lower() for _, (holder, field, _) in files}
        # A file system that does not tell capitals apart has to be able to hold the tree.
        owners: dict[str, list[dict]] = {}
        for unit, (_, _, target) in files:
            owners.setdefault(target.lower(), []).append(unit)
        lowered = set()
        for who in owners.values():
            for unit in who if len(who) > 1 else ():
                if id(unit) not in lowered and unit["at"] + 1 < len(unit["choices"]):
                    unit["at"] += 1
                    lowered.add(id(unit))
        for unit, (holder, field, target) in () if lowered else files:
            last = len(unit["choices"]) - 1
            if id(unit) in lowered or unit["at"] == last or holder[field] == target or target.lower() in ours:
                continue
            key = (holder[field], target)
            if key not in looked:
                path, source = on_disk(directory, target), on_disk(directory, holder[field])
                try:
                    looked[key] = path.exists() and path.read_bytes() != source.read_bytes()
                except OSError:
                    looked[key] = True
            if looked[key]:                 # somebody's file, not the manifest's: nothing is renamed onto it
                unit["at"] = unit["default"] if unit["at"] < unit["default"] else last
                lowered.add(id(unit))
                remarks.append(f"{target} is in the way, and the manifest does not know it: {holder[field]} "
                               f"{'keeps the name it has' if unit['at'] == last else 'goes without its name'}. "
                               f"Move it away and run `gb_data.py names` again")
        if not lowered:
            return [entry for _, entry in files]


def undo(directory: Path, steps: list) -> None:
    """Take renamings back, the last first: each file that is where a step put it and not where it was."""
    for step in reversed(steps):
        if not isinstance(step, list) or len(step) != 2 or not safe(step[0]) or not safe(step[1]):
            continue
        source, target = on_disk(directory, step[0]), on_disk(directory, step[1])
        try:
            if target.exists() and not source.exists():
                target.replace(source)
        except OSError:
            pass


def recover(directory: Path) -> None:
    """Undo a renaming that was cut short before the manifest was written, so that the files
    are called what the manifest calls them. Nearly always there is nothing to undo."""
    journal = directory / JOURNAL
    if not journal.exists():
        return
    try:
        record = json.loads(journal.read_text(encoding="utf-8"))
        if record["manifest"] == sha256((directory / "manifest.json").read_bytes()):
            undo(directory, record["steps"])
    except (OSError, ValueError, KeyError, TypeError):
        pass
    journal.unlink(missing_ok=True)


def written(directory: Path) -> None:
    """The manifest has been written with the new names in it: the renaming is over."""
    (directory / JOURNAL).unlink(missing_ok=True)


def rename(directory: Path, moves: list[tuple[str, str]]) -> str | None:
    """Rename files, all of them or none. Returns what went wrong, or None.

    The steps are written down first. If this is cut short, the next command that reads the
    tree takes them back (`recover`): a manifest that names files that are not there would
    stop every command."""
    sources = {source.lower() for source, _ in moves}
    aside, steps = [], []
    for source, target in moves:
        if target.lower() in sources:       # a name another file has now, or its own in other capitals
            aside.append([source, target + ".renaming"])
            source = target + ".renaming"
        steps.append([source, target])
    steps = aside + steps
    try:
        manifest = sha256((directory / "manifest.json").read_bytes())
    except OSError:
        manifest = ""
    done = []
    try:
        (directory / JOURNAL).write_text(json.dumps({"manifest": manifest, "steps": steps}), encoding="utf-8")
        for source, target in steps:
            on_disk(directory, source).replace(on_disk(directory, target))
            done.append([source, target])
    except (OSError, KeyboardInterrupt) as error:
        undo(directory, done)
        (directory / JOURNAL).unlink(missing_ok=True)
        if isinstance(error, KeyboardInterrupt):
            raise
        return f"cannot rename {error.filename or directory / JOURNAL} ({error.strerror or error})"
    return None


def name_strings(directory: Path, region: dict, view: dict, labels: list[list]) -> tuple[int, int]:
    """Write the name of each string of a file of text over its line, and have the file's first
    line say what the region's file is called now. Returns how many strings start at a name,
    and how many more start after one. Only those comments change: an edited line stays."""
    offsets = [offset for offset, _ in labels]
    comments: dict[int, str] = {}
    at_name = after_name = 0
    for offset, _ in view.get("strings", ()):
        index = bisect_right(offsets, offset) - 1
        if index < 0:
            continue
        behind = offset - offsets[index]
        at_name += not behind
        after_name += bool(behind)
        comments[offset] = (f"# {region['bank']:02X}:{region['address'] + offset:04X} {labels[index][1]}"
                            + (f"+{behind}" if behind else ""))
    path = on_disk(directory, view["file"])
    try:
        raw = path.read_bytes()
        lines = raw.decode("utf-8").splitlines(keepends=True)
    except (OSError, UnicodeDecodeError):
        return at_name, after_name          # `build` says what is wrong with it
    out = []
    for number, line in enumerate(lines):
        if STRING_NAME.fullmatch(line.rstrip()):
            continue
        if number == 0:
            line = TEXT_HEAD.sub(lambda head: head.group(1) + region["file"] + head.group(3), line, count=1)
        string = STRING_LINE.match(line)
        if string and int(string.group(1), 16) in comments:
            out.append(comments[int(string.group(1), 16)] + ("\r\n" if line.endswith("\r\n") else "\n"))
        out.append(line)
    fresh = "".join(out).encode("utf-8")
    if fresh != raw:
        try:
            path.write_bytes(fresh)
        except OSError:
            return at_name, after_name
        if sha256(raw) == view.get("sha256"):       # as `text` wrote it: still nobody's edit
            view["sha256"] = sha256(fresh)
    return at_name, after_name


def apply(directory: Path, manifest: dict) -> dict | None:
    """Call every file of a tree what names.sym calls it, and say so in `manifest`, which the
    caller writes straight afterwards (and then calls `written`). Returns what was done, or
    None for a tree nobody has named, which is not touched."""
    recover(directory)
    if not (directory / NAMES).exists() and "names" not in manifest and \
            not any("labels" in region for region in manifest["regions"]):
        return None
    _, _, names, remarks = read_names(directory)
    section = manifest.get("names", {})
    # The names that lie in each region, with how far in: one to an address, the first line's.
    starts, streams = tree_places(manifest)
    inside: list[list[list]] = [[] for _ in starts]
    elsewhere = 0
    for where in sorted(names, key=rom_offset):
        offset = rom_offset(where)
        index = bisect_right(starts, offset) - 1
        if offset >= manifest["rom"]["size"]:
            elsewhere += 1
            continue
        inside[index].append([offset - starts[index], names[where][0]])
        elsewhere += manifest["regions"][index]["kind"] != "data" and where not in streams
    units = units_of(manifest, names, inside)
    files = settle(directory, units, remarks)
    moves = [(holder[field], target) for holder, field, target in files if holder[field] != target]
    there = [move for move in moves if on_disk(directory, move[0]).exists()]
    # A file of tile numbers has a picture beside it that the manifest does not name (`maps` draws it from
    # the file's name): it goes where the file goes, or it is left behind under the old name.
    beside = [(source[:-len(".txt")] + ".png", target[:-len(".txt")] + ".png") for source, target in there
              if source.endswith(".map.txt") and target.endswith(".map.txt")]
    beside = [move for move in beside
              if on_disk(directory, move[0]).exists() and not on_disk(directory, move[1]).exists()]
    report: dict = {"renamed": len(there), "failed": rename(directory, there + beside) if there else None}
    if report["failed"]:                    # nothing was renamed, and the manifest is left saying what it said
        print(f"gb_data: names: {report['failed']}: every file keeps the name it had", file=sys.stderr)
        return report
    for holder, field, target in files:
        holder[field] = target
    # What each renamed file was called: the name it gets back, and its address is in that.
    was = {}
    for unit in units:
        if unit["what"] == "region" and unit["choices"][unit["at"]] != unit["old"]:
            was[unit["region"]["file"]] = unit["folder"] + unit["old"] + unit["kind"]
        for entry in unit["things"] if unit["what"] == "stream" else ():
            if entry["thing"]["made"] != f"{entry['folder']}{entry['old']}.made":
                was[entry["thing"]["made"]] = f"{entry['folder']}{entry['old']}.made"
    manifest.pop("names", None)
    if was or section.get("seen"):
        manifest["names"] = dict({"seen": True} if section.get("seen") else {}, was=dict(sorted(was.items())))
    # Each region's names into the manifest, and the strings' names into the files of text.
    counts = {"regions": [0, 0], "data": [0, 0], "views": [0, 0], "strings": [0, 0, 0], "things": [0, 0],
              "streams": [0, 0], "seen": 0, "several": 0, "and_more": 0, "lines": len(names), "elsewhere": elsewhere,
              "by_hand": sum(1 for _, source in names.values() if not source)}
    for unit in units:
        by_label = unit["by_label"] and unit["at"] < unit["default"]
        if unit["what"] == "region":
            region = unit["region"]
            labels = inside[bisect_right(starts, region["offset"]) - 1]
            region.pop("labels", None)
            if labels:
                region["labels"] = labels
            data = region["kind"] == "data"
            for key, count in (("regions", 1), ("data", data), ("views", len(unit["follows"]))):
                counts[key][0] += by_label * count
                counts[key][1] += count
            counts["seen"] += not unit["by_label"] and unit["at"] < unit["default"]
            counts["several"] += data and len(labels) > 1
            counts["and_more"] += by_label and len(labels) > 1
            for view in region.get("views", ()):
                if isinstance(view, dict) and view.get("kind") == "text" and isinstance(view.get("file"), str):
                    at_name, after_name = name_strings(directory, region, view, labels)
                    counts["strings"][0] += at_name
                    counts["strings"][1] += after_name
                    counts["strings"][2] += len(view.get("strings", ()))
        else:
            for key, count in (("views", 1),) if unit["what"] == "view" else (("things", len(unit["things"])),
                                                                             ("streams", 1)):
                counts[key][0] += by_label * count
                counts[key][1] += count
    report.update(counts)
    for remark in remarks:
        print(f"gb_data: names: {remark}", file=sys.stderr)
    return report


def said(report: dict) -> list[str]:
    """What `names` prints of what `apply` did."""
    def of(pair: list[int]) -> str:
        return f"{pair[0]:,} of {pair[1]:,}"

    lines = [f"{NAMES}: {report['lines']:,} {'address has' if report['lines'] == 1 else 'addresses have'} a name, "
             f"{report['by_hand']:,} written by hand and {report['lines'] - report['by_hand']:,} brought in"]
    lines.append(f"  regions   {of(report['data'])} files of data are called by a name (a file of code or the header "
                 f"keeps its own)")
    if report["and_more"]:
        lines.append(f"            {report['and_more']:,} of them hold other names as well, and say how many: "
                     f"Label_and_N_more")
    if report["seen"]:
        lines.append(f"            {report['seen']:,} more of data are called by what the game was seen to use them "
                     f"as")
    if report["views"][1]:
        lines.append(f"  views     {of(report['views'])} pictures and files of text are called by a name")
    if report["strings"][2]:
        at_name, after_name, strings = report["strings"]
        lines.append(f"  strings   {at_name:,} of {strings:,} start at a name, and {after_name:,} more after one in "
                     f"their region (Label+N): written over each in its file of text")
    if report["things"][1]:
        lines.append(f"  unpacked  {of(report['things'])} things are called by a name, of {of(report['streams'])} "
                     f"streams: the ways a stream was unpacked share its name")
    if report["several"]:
        lines.append(f"  {report['several']:,} data files have more than one name in them: `labels` in the manifest "
                     f"lists them for each region, for whoever draws a view there")
    if report["elsewhere"]:
        lines.append(f"  {report['elsewhere']:,} of the names are in code or the header, or not in this ROM, and name "
                     f"no file")
    lines.append(f"  {report['renamed']:,} {'file' if report['renamed'] == 1 else 'files'} renamed")
    return lines


def write_list(path: Path, manifest: dict) -> None:
    """Everything the tree has, with its address, its name and its file: for a person to search."""
    lines = ["# What is called what in this data tree (gb_data.py names --list). Under each region are",
             "# its views, and the names that lie further in with how far. `-` is no name.", ""]
    for region in manifest["regions"]:
        labels = {offset: label for offset, label in region.get("labels", ())}
        lines.append(f"{region['bank']:02X}:{region['address']:04X} {region['kind']} {labels.get(0, '-')} "
                     f"{region['file']} ({region['size']} bytes)")
        for view in region.get("views", ()):
            offset = view.get("offset", 0)
            lines.append(f"  {region['bank']:02X}:{region['address'] + offset:04X} {view.get('kind')} "
                         f"{labels.get(offset, '-')} {view.get('file')}")
            for start, size in view.get("strings", ()) if view.get("kind") == "text" else ():
                lines.append(f"    {region['bank']:02X}:{region['address'] + start:04X} string "
                             f"{labels.get(start, '-')} +{start:04X} ({size} bytes)")
        for offset, label in region.get("labels", ()):
            if offset:
                lines.append(f"  {region['bank']:02X}:{region['address'] + offset:04X} label {label} +{offset:04X}")
    for thing in manifest.get("unpacked", ()):
        lines.append(f"{thing['bank']:02X}:{thing['address']:04X} unpacked by {thing['unpacker']}: {thing['file']}")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    except OSError as error:
        raise NamesError(f"cannot write {path}: {error}") from error
