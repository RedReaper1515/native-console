#!/usr/bin/env python3
"""A data tree split again, with what was done to the old one kept.

`gb_data.py` uses this for `split --keep OLD`, and before `split --force`
writes over a tree.

Splitting again is ordinary work. Every time the analysis is told more of
what is data (`gb_data.py annotations`), its export changes, and with it
where the regions are cut. A tree that was split before has had work done in
it since: files edited, tiles drawn and painted, strings written out and
changed, what the game unpacks harvested and redrawn, a charmap read off a
sheet. `split EXPORT --rom ROM -o NEW --keep OLD` writes the new export's
tree into a new folder with all of that taken over. The old tree is read and
never written to.

Everything is carried by the bytes of the ROM it is of, not by the file it
was in:

- **Edited bytes.** The image `build` makes of the new tree is the image it
  makes of the old one. A byte that was edited and is data in the new tree
  is edited there. One that is code or the header in the new tree cannot be:
  the command stops and says where, and `--lose-edits-in-code` goes on
  without those bytes, naming them.
- **Pictures of tiles** go with their bytes. Where all of a picture's tiles
  lie in one data file of the new tree, the picture is copied as it is, the
  modder's file to the byte, and recorded on that file with its new offset.
- **Text** goes a string at a time. A string that lies in one data file of
  the new tree is in that file's text, which is written the way `text`
  writes it, with every string that was edited saying what the modder made
  it say and every comment line where it was. Strings from one old file may
  end up in several, and from several in one.
- **What the game unpacks**, `unpackers.json`, `charmap.json` and
  `glyphs.png` do not depend on where the regions are cut, and are copied.
- **What `uses` wrote** is of the bytes of data files. Where every data byte
  of the new tree was data in the old one it is carried exactly: `seen` cut
  to the new regions, and `readers` added up again from it. Where the new
  tree has data the old one had as code, the old manifest says nothing of
  those bytes, so nothing is carried and `uses` has to be run again with the
  records.
- **A view that cannot go with its bytes** (a picture whose tiles are in two
  files now, or partly code) is left behind and named, with the reason. An
  edit made through it is not lost: it is in the bytes of the new files.
- **What this tool does not know**: a view of a kind it has no rule for, a
  field of a region or of the manifest that it does not write itself, a
  file's name that is not the one `split` gives. Each is carried where the
  region it belongs to is the same bytes in the new tree, and named where it
  is not. A file at the top of the old tree that the manifest does not name
  is copied as it is and named; one further down is left and counted.

When the tree is written it is read back and put together, and that has to
give the image and the replacements the old tree gives. Otherwise the
command fails and says the new tree is not to be used.
"""
from __future__ import annotations

import bisect
from pathlib import Path

import gb_data_text
import gb_unpacked

# What `split`, `uses`, `tiles` and `text` write on a region and in the manifest. Anything else was
# put there by something this module knows nothing of.
REGION_FIELDS = {"offset", "size", "bank", "address", "kind", "guess", "file", "sha256", "referenced_by", "seen",
                 "views"}
MANIFEST_FIELDS = {"schema", "version", "rom", "regions", "readers", "unpacked"}
TOP_FILES = (gb_unpacked.DECLARATION, "charmap.json", "glyphs.png")
USES = ("tiles", "tile_map", "oam", "sound", "palette", "bank", "jump", "pointer")
NAMED = 12                      # how many of a kind of thing are named before the rest are counted


def counted(count: int, one: str, many: str | None = None) -> str:
    return f"{count:,} {one if count == 1 else many or one + 's'}"


def at(offset: int) -> str:
    bank = offset // 0x4000
    return f"{bank:02X}:{offset if bank == 0 else 0x4000 + offset % 0x4000:04X}"


def stem(region: dict) -> str:
    name = region["file"]
    return name[:-len(".bin")] if name.endswith(".bin") else name


def written_over(tool, directory: Path) -> list[str]:
    """Before `split --force` writes over a tree: stop if the tree has edits,
    which are what `status` lists, and otherwise say what else it holds that
    the new one will not."""
    try:
        manifest, loaded = tool.load_tree(directory)
        _, redrawn, disagree = tool.with_views(directory, loaded)
        _, replaced = tool.with_unpacked(directory, manifest, tool.tree_unpackers(directory, manifest))
    except tool.DataError as error:
        raise tool.DataError(f"{directory} holds a data tree that cannot be read ({error}), so nothing says whether "
                             f"it has edits: --discard-edits writes over it whatever it holds") from error
    lines = [tool.describe(region) for region, _ in tool.edited(loaded)] + redrawn + disagree + replaced
    if lines:
        more = f", and {len(lines) - 1} more" if len(lines) > 1 else ""
        raise tool.DataError(f"{directory} has edits ({lines[0]}{more}), and --force does not write over them: "
                             f"split into a new folder with `-o NEW --keep {directory}` to take them along, or give "
                             f"--discard-edits to lose them")
    views = sum(len(region.get("views", ())) for region, _ in loaded)
    things = len(manifest.get("unpacked", ()))
    gone = ([counted(views, "view") + " (pictures and text)"] if views else []) + \
           ([counted(things, "thing") + " the game unpacks"] if things else []) + \
           (["what `uses` wrote"] if "readers" in manifest else [])
    return gone


def seen_again(loaded: list[tuple[dict, bytes]], regions: list[dict]) -> tuple[dict[int, list[dict]], list[dict]]:
    """What `uses` wrote, for regions cut another way: each new data region's
    `seen` by its place in `regions`, and `readers`, both as `uses` would
    write them from the same records. Only right where every data byte of
    the new regions was data before."""
    starts = [region["offset"] for region in regions]
    seen: dict[int, list[dict]] = {}
    for region, _ in loaded:
        for run in region.get("seen", ()):
            first, end = region["offset"] + run["offset"], region["offset"] + run["offset"] + run["size"]
            while first < end:
                index = bisect.bisect_right(starts, first) - 1
                new = regions[index]
                stop = min(end, new["offset"] + new["size"])
                if new["kind"] == "data":
                    runs = seen.setdefault(index, [])
                    last = runs[-1] if runs else None
                    if last and last["offset"] + last["size"] == first - new["offset"] and \
                            all(last[field] == run[field] for field in ("as_it_is", "worked_out", "read_at",
                                                                         "one_reader")):
                        last["size"] += stop - first
                    else:
                        runs.append({"offset": first - new["offset"], "size": stop - first,
                                     "as_it_is": list(run["as_it_is"]), "worked_out": list(run["worked_out"]),
                                     "read_at": list(run["read_at"]), "one_reader": run["one_reader"]})
                first = stop
    readers: dict[tuple, dict[str, int]] = {}
    for index in sorted(seen):
        for run in seen[index]:
            what = "+".join(use for use in USES if use in run["as_it_is"]) or \
                ("(" + "+".join(use for use in USES if use in run["worked_out"]) + ")" if run["worked_out"]
                 else "nothing")
            by_reader = readers.setdefault(tuple(run["read_at"]), {})
            by_reader[what] = by_reader.get(what, 0) + run["size"]
    ranked = sorted(readers.items(), key=lambda item: (-sum(item[1].values()), item[0]))
    return seen, [{"bank": bank, "address": address, "bytes": sum(uses.values()),
                   "uses": dict(sorted(uses.items(), key=lambda item: -item[1]))}
                  for (bank, address), uses in ranked]


def comments_of(text: str, header: str) -> tuple[dict[int, list[str]], dict[int, list[str]]]:
    """The comment lines somebody put into a file of text: those in front of
    each string's line, and those after the last, by the string's offset.
    Not the lines `text` itself writes at the top."""
    if text.startswith(header):
        text = text[len(header):]
    before: dict[int, list[str]] = {}
    after: dict[int, list[str]] = {}
    pending: list[str] = []
    last = None
    for line in text.splitlines():
        match = gb_data_text.LINE.match(line)
        if match:
            last = int(match.group(1), 16)
            if pending:
                before[last] = pending
            pending = []
        elif line.startswith("#"):
            pending.append(line)
    if pending and last is not None:
        after[last] = pending
    return before, after


def split_keeping(tool, export: dict, rom: bytes, regions: list[dict], out: Path, old: Path, lose: bool) -> int:
    """Write the tree of `regions` to `out` with what the tree at `old` holds."""
    sha256 = tool.sha256
    here, there = out.resolve(), old.resolve()
    if here == there or here in there.parents or there in here.parents:
        raise tool.DataError(f"--keep reads {old} and writes a new tree, and the two cannot be one folder or one "
                             f"inside the other: give -o a folder of its own")
    manifest, loaded = tool.load_tree(old)
    if manifest["rom"]["sha256"] != sha256(rom):
        raise tool.DataError(f"{old} is a tree of another ROM")
    tool.refuse_pinned(tool.edited(loaded))
    contents, _, disagree = tool.with_views(old, loaded)
    if disagree:
        raise tool.DataError(f"{disagree[0]}: keep one of the two in {old} before splitting again (`tiles --force` "
                             f"draws a picture again from the file, and `text --force` writes the text again)")
    unpackers = tool.tree_unpackers(old, manifest)
    replacements, _ = tool.with_unpacked(old, manifest, unpackers)
    files = b"".join(content for _, content in loaded)      # the old tree's files, one after another
    image = b"".join(contents)                              # and what `build` makes of the old tree
    kept: list[str] = []
    left: list[str] = []

    starts = [region["offset"] for region in regions]
    old_starts = [region["offset"] for region, _ in loaded]

    def new_at(offset: int) -> tuple[int, dict]:
        index = bisect.bisect_right(starts, offset) - 1
        return index, regions[index]

    def whole_in_data(first: int, end: int) -> tuple[int, dict] | None:
        """The new data region a run of bytes lies in whole, with its place in `regions`."""
        index, region = new_at(first)
        return (index, region) if region["kind"] == "data" and end <= region["offset"] + region["size"] else None

    def why_not(first: int, end: int) -> str:
        """Why a run of bytes that is in no one data file of the new tree is not."""
        pieces = []
        offset = first
        while offset < end:
            _, region = new_at(offset)
            pieces.append(region)
            offset = region["offset"] + region["size"]
        if all(region["kind"] == "data" for region in pieces):
            return f"its bytes lie in {len(pieces)} files of the new tree"
        pinned = sum(min(end, region["offset"] + region["size"]) - max(first, region["offset"])
                     for region in pieces if region["kind"] != "data")
        return (f"{counted(pinned, 'of its bytes is', 'of its bytes are')} code or the header in the new tree"
                if pinned < end - first else "its bytes are code or the header in the new tree")

    # An edit to bytes that are code in the new tree has nowhere to go: no image may change code.
    target = bytearray(image)
    lost = []
    for region in regions:
        first, end = region["offset"], region["offset"] + region["size"]
        if region["kind"] not in tool.PINNED or image[first:end] == rom[first:end]:
            continue
        differ = [offset for offset in range(first, end) if image[offset] != rom[offset]]
        before = loaded[bisect.bisect_right(old_starts, differ[0]) - 1][0]
        what = "code" if region["kind"] == "code" else "the cartridge header"
        lost.append(f"{counted(len(differ), 'edited byte')} of {before['file']}, from {at(differ[0])} on: "
                    f"{'it is' if len(differ) == 1 else 'they are'} {what} in the new tree")
        target[first:end] = rom[first:end]
    if lost and not lose:
        more = f" (and {len(lost) - 1} more such)" if len(lost) > 1 else ""
        raise tool.DataError(f"{lost[0]}{more}. An image cannot change code, so the edit would be lost. Tell the "
                             f"analysis those bytes are data (a `data` line in its annotations) and make the export "
                             f"again, or go on without the edit with --lose-edits-in-code")

    # Regions that are the same bytes in both trees: what this tool does not know stays with them.
    places = {(region["offset"], region["size"], region["kind"]): region for region, _ in loaded}
    same_cut = len(regions) == len(loaded) and all((region["offset"], region["size"], region["kind"]) in places
                                                    for region in regions)
    named = 0
    for region in regions:
        before = places.pop((region["offset"], region["size"], region["kind"]), None)
        region["file"] = tool.file_name(region)
        region["sha256"] = sha256(rom[region["offset"]:region["offset"] + region["size"]])
        if before is None:
            continue
        if before["file"] != tool.file_name(before):
            region["file"] = before["file"]     # somebody named it
            named += 1
        for field in sorted(set(before) - REGION_FIELDS):
            region[field] = before[field]
    for before in places.values():              # the old regions with no such twin
        odd = sorted(set(before) - REGION_FIELDS)
        if before["file"] != tool.file_name(before):
            left.append(f"the name of {before['file']}: its bytes ({at(before['offset'])}, {before['size']}) are "
                        f"cut another way in the new tree")
        if odd:
            left.append(f"{', '.join(repr(field) for field in odd)} of {before['file']}, which this gb_data.py does "
                        f"not know how to move: its bytes are cut another way in the new tree")
    if named:
        kept.append(f"the names of {counted(named, 'file')} that had been given one")
    extra = sorted(set(manifest) - MANIFEST_FIELDS)
    for field in extra:
        if same_cut:
            kept.append(f"'{field}' of the manifest, as it is")
        else:
            left.append(f"'{field}' of the manifest, which this gb_data.py does not know: the regions are cut "
                        f"another way, and it may speak of them")

    # The views. `file_bytes` is what the new tree's files hold: the image the old tree builds,
    # but under a view that is carried the old file's own bytes, with the edit left in the view.
    file_bytes = bytearray(target)
    written: dict[str, bytes] = {}              # every file of the new tree but the regions', by name
    views: dict[int, list[dict]] = {}           # by a region's place in `regions`
    strings: dict[int, list[tuple]] = {}        # the same, for text: (offset in the ROM, size, line, comments)
    charmaps: dict[str, dict[int, str] | str] = {}
    carried = {"tiles": [0, 0], "text": [0, 0], "other": [0, 0]}    # how many, and how many of them edited

    def put(name: str, content: bytes) -> None:
        if name in written:
            raise tool.DataError(f"two files of the new tree would both be {name}")
        written[name] = content

    for region, _ in loaded:
        base = region["offset"]
        index, new = new_at(base)
        in_place = index if (new["offset"], new["size"], new["kind"]) == (base, region["size"], "data") else None
        for view in region.get("views", ()):
            if view["kind"] == "tiles":
                first = base + view["offset"]
                end = first + view["tiles"] * 8 * view["bpp"]
                found = whole_in_data(first, end)
                was_edited = image[first:end] != files[first:end]
                if found is None:
                    left.append(f"{view['file']}, the picture of {counted(view['tiles'], 'tile')} at {at(first)}: "
                                f"{why_not(first, end)}"
                                + (". What was painted in it is in the bytes of the new files" if was_edited else ""))
                    continue
                index, new = found
                offset = first - new["offset"]
                name = (f"{stem(new)}.tiles.png" if offset == 0 else
                        f"{stem(new)}.tiles_{new['address'] + offset:04X}.png")
                put(name, tool.inside(old, view["file"]).read_bytes())
                views.setdefault(index, []).append(dict(view, file=name, offset=offset))
                file_bytes[first:end] = files[first:end]
                carried["tiles"][0] += 1
                carried["tiles"][1] += was_edited
            elif view["kind"] == "text":
                charmap = view["charmap"]
                if charmap not in charmaps:
                    try:
                        charmaps[charmap] = gb_data_text.load_charmap(tool.inside(old, charmap))
                    except gb_data_text.TextError as error:
                        charmaps[charmap] = str(error)
                letters = charmaps[charmap]
                if isinstance(letters, str):
                    left.append(f"{view['file']}, the text of {region['file']}: {letters}")
                    continue
                text = tool.inside(old, view["file"]).read_bytes().decode("utf-8-sig")
                said = gb_data_text.read_strings(view["file"], text)
                header = gb_data_text.strings_text(region["file"], b"", [], letters)
                before, after = comments_of(text, header)
                missed = []
                waiting: list[str] = []         # the comments of strings left behind go to the next one kept
                placed = None
                for offset, size in view["strings"]:
                    first, end = base + offset, base + offset + size
                    found = whole_in_data(first, end)
                    was_edited = image[first:end] != files[first:end]
                    why = None
                    if found is None:
                        why = why_not(first, end)
                    elif strings.get(found[0]) and strings[found[0]][0][4] != charmap:
                        why = (f"the text of {regions[found[0]]['file']} is written with another charmap, "
                               f"{strings[found[0]][0][4]}, and a file of text has one")
                    if why:
                        missed.append((first, why, was_edited))
                        waiting += before.get(offset, []) + after.get(offset, [])
                        continue
                    # The line as it was, where it says what `build` made of it. A line the old
                    # tree let stand against an edited file says something else, and is written
                    # again from the file, as the others of an unedited file of text are.
                    line = said[offset][1] if offset in said and said[offset][0] == size else None
                    if line is not None and not was_edited:
                        try:
                            if gb_data_text.encode(line, letters) != files[first:end]:
                                line = None
                        except gb_data_text.TextError:
                            line = None
                    placed = (first, size, line, (waiting + before.get(offset, []), list(after.get(offset, []))),
                              charmap)
                    waiting = []
                    strings.setdefault(found[0], []).append(placed)
                    file_bytes[first:end] = files[first:end]
                    carried["text"][1] += was_edited
                if placed is not None:
                    placed[3][1].extend(waiting)
                if missed:
                    edited = sum(1 for _, _, was_edited in missed if was_edited)
                    left.append(f"{counted(len(missed), 'string')} of {view['file']}, the first at "
                                f"{at(missed[0][0])}: {missed[0][1]}"
                                + (f". What {counted(edited, 'of them was', 'of them were')} changed to is in the "
                                   f"bytes of the new files" if edited else ""))
            elif in_place is not None:
                # A kind of view with no rule here goes where its region is the same bytes, and
                # the region's file then holds what the old one held.
                if isinstance(view.get("file"), str) and view["file"] not in written:
                    put(view["file"], tool.inside(old, view["file"]).read_bytes())
                views.setdefault(in_place, []).append(dict(view))
                file_bytes[base:base + region["size"]] = files[base:base + region["size"]]
                carried["other"][0] += 1
            else:
                left.append(f"{view.get('file', 'a view')}, a view of kind '{view['kind']}' of {region['file']}, "
                            f"which this gb_data.py does not know how to move: its bytes are cut another way in the "
                            f"new tree. An edit made through it is in the bytes of the new files")

    for index, found in strings.items():   # each file's strings, in the order they lie in it
        new = regions[index]
        base = new["offset"]
        letters = charmaps[found[0][4]]
        content = bytes(file_bytes[base:base + new["size"]])
        placed = [(first - base, size) for first, size, _, _, _ in found]
        fresh = gb_data_text.strings_text(new["file"], content, placed, letters)
        lines = fresh.split("\n")
        head, body = lines[:len(lines) - len(found) - 1], lines[len(lines) - len(found) - 1:-1]
        for (first, size, line, (before, after), _), written_line in zip(found, body):
            head += before
            head.append(written_line if line is None else f"+{first - base:04X} {size} |{line}|")
            head += after
        name = f"{stem(new)}.text.txt"
        put(name, ("\n".join(head) + "\n").encode("utf-8"))
        views.setdefault(index, []).append({
            "kind": "text", "file": name, "charmap": found[0][4],
            "strings": [[offset, size] for offset, size in placed],
            "sha256": sha256(fresh.encode("utf-8")),
            "data_sha256": sha256(b"".join(content[offset:offset + size] for offset, size in placed))})
        carried["text"][0] += 1

    def first_byte(view: dict) -> tuple:
        try:
            return (0, tool.spans(view)[0])
        except (KeyError, TypeError, IndexError):
            return (1, ())                      # a kind `spans` does not know: after the others, as they came

    for index, found in views.items():
        regions[index]["views"] = sorted(found, key=first_byte)

    # What `uses` wrote is of data bytes: carried whole where the old tree knows all of the new one's.
    if "readers" in manifest:
        was_data = bytearray(len(rom))
        for region, _ in loaded:
            if region["kind"] == "data":
                was_data[region["offset"]:region["offset"] + region["size"]] = b"\x01" * region["size"]
        unknown = sum(region["size"] - was_data.count(1, region["offset"], region["offset"] + region["size"])
                      for region in regions if region["kind"] == "data")
        if unknown:
            readers = None
            left.append(f"what `uses` wrote (`seen` and `readers`): {counted(unknown, 'byte')} that "
                        f"{'is' if unknown == 1 else 'are'} data in the new tree "
                        f"{'was' if unknown == 1 else 'were'} in files of code in the old one, and its manifest does "
                        f"not say what the game did with {'it' if unknown == 1 else 'them'}. Run `uses` again with "
                        f"the records")
        else:
            seen, readers = seen_again(loaded, regions)
            for index, runs in seen.items():
                regions[index]["seen"] = runs
            kept.append("what `uses` wrote (`seen` cut to the new regions, and `readers` added up again from it)")
    else:
        readers = None

    # What does not depend on where the regions are cut.
    things = manifest.get("unpacked", [])
    for thing in things:
        for name in (thing["made"], thing["file"]):
            put(name, tool.inside(old, name).read_bytes())
    known = {"manifest.json", "manifest.json.new"} | {region["file"] for region, _ in loaded} | \
        {view["file"] for region, _ in loaded for view in region.get("views", ()) if isinstance(view.get("file"), str)}
    region_files = {region["file"] for region in regions}
    copied, strays = [], []
    for path in sorted(old.rglob("*")):
        name = path.relative_to(old).as_posix()
        if not path.is_file() or name in known or name in written:
            continue
        if "/" in name or name in region_files:
            strays.append(name)
            continue
        put(name, path.read_bytes())
        if name not in TOP_FILES:
            copied.append(name)
    if copied:
        kept.append(f"{', '.join(copied[:NAMED])}{f' and {len(copied) - NAMED} more' if len(copied) > NAMED else ''}"
                    f", copied as {'it is' if len(copied) == 1 else 'they are'}: nothing here knows what "
                    f"{'it says' if len(copied) == 1 else 'they say'}"
                    + ("" if same_cut else ", and what names a file or an offset of the old tree has to be looked at"))
    if strays:
        left.append(f"{counted(len(strays), 'file')} of {old} that its manifest does not name ({', '.join(strays[:3])}"
                    f"{', ...' if len(strays) > 3 else ''}): left where "
                    f"{'it is' if len(strays) == 1 else 'they are'}")

    # Written: the regions, then everything else, and the manifest last.
    new_manifest = {
        "schema": tool.SCHEMA, "version": 1,
        "rom": {"size": len(rom), "sha1": manifest["rom"]["sha1"], "sha256": sha256(rom),
                "title": export["rom"].get("title", "")},
        "regions": regions,
    }
    if readers is not None:
        new_manifest["readers"] = readers
    if "unpacked" in manifest:
        new_manifest["unpacked"] = things
    if same_cut:
        for field in extra:
            new_manifest[field] = manifest[field]
    taken = set()
    for region in regions:
        first, end = region["offset"], region["offset"] + region["size"]
        if region["file"] in taken or region["file"] in written:
            raise tool.DataError(f"two files of the new tree would both be {region['file']}")
        taken.add(region["file"])
        target_file = tool.inside(out, region["file"])
        target_file.parent.mkdir(parents=True, exist_ok=True)
        target_file.write_bytes(file_bytes[first:end])
    for name, content in written.items():
        target_file = tool.inside(out, name)
        target_file.parent.mkdir(parents=True, exist_ok=True)
        target_file.write_bytes(content)
    tool.write_manifest(out, new_manifest)

    # Read back and put together: the new tree has to build what the old one builds.
    again, again_loaded = tool.load_tree(out)
    built, _, disagree = tool.with_views(out, again_loaded)
    pack, _ = tool.with_unpacked(out, again, tool.tree_unpackers(out, again))
    if disagree or b"".join(built) != bytes(target) or pack != replacements:
        raise tool.DataError(f"the tree written to {out} does not build what {old} builds"
                             + (f" ({disagree[0]})" if disagree else "") +
                             f": that is a fault in gb_data_keep.py, and {out} is not to be used")

    counts: dict[str, list[int]] = {}
    for region in regions:
        entry = counts.setdefault(region["kind"], [0, 0])
        entry[0] += 1
        entry[1] += region["size"]
    print(f"wrote {len(regions)} files and manifest.json to {out}, with what {old} holds")
    for kind in ("data", "code", "header"):
        if kind in counts:
            print(f"  {kind:6s} {counts[kind][0]:6,d} files, {counts[kind][1]:9,d} bytes")
    changed = sum(1 for offset in range(len(rom)) if target[offset] != rom[offset]) if bytes(target) != rom else 0
    in_files = sum(1 for region in regions
                   if file_bytes[region["offset"]:region["offset"] + region["size"]] !=
                   rom[region["offset"]:region["offset"] + region["size"]])
    edited_things = len(replacements)
    said = []
    if in_files:
        said.append(f"{counted(in_files, 'edited file')}")
    if carried["tiles"][0]:
        said.append(f"{counted(carried['tiles'][0], 'picture')} of tiles ({carried['tiles'][1]} edited)")
    if carried["text"][0]:
        total = sum(len(found) for found in strings.values())
        said.append(f"{counted(carried['text'][0], 'file')} of text with {counted(total, 'string')} "
                    f"({carried['text'][1]} edited)")
    if carried["other"][0]:
        said.append(f"{counted(carried['other'][0], 'view')} of other kinds, where they were")
    if things:
        said.append(f"{counted(len(things), 'thing')} the game unpacks ({edited_things} "
                    f"{'replacement' if edited_things == 1 else 'replacements'} in the pack)")
    said += [name for name in TOP_FILES if name in written]
    print("kept:")
    for line in said + kept:
        print(f"  {line}")
    if not said and not kept:
        print("  nothing: the old tree held nothing but the ROM")
    if left or lost:
        print("not kept:")
        for line in lost:
            print(f"  {line}. The edit is lost (--lose-edits-in-code)")
        for line in left:
            print(f"  {line}")
    print(f"`build` makes of {out} what it makes of {old}"
          + (", but for the edits that were lost" if lost else "") + ": "
          + (f"an image with {counted(changed, 'byte')} edited" if changed else "the original ROM")
          + (f", and a pack with {counted(edited_things, 'replacement')}" if unpackers is not None else ""))
    return 0
