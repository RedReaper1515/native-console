#!/usr/bin/env python3
"""A game's tiles as pictures, found from where a played game put them.

`gb_data.py` uses this when `uses` writes what was seen into a tree, and for
`tiles DIR` with no file named.

`gb_data.py tiles DIR FILE` draws tiles where it is told they are: the file,
how far in they start, how many, and whether a tile is 16 bytes (2 bits a
pixel) or 8 (1 bit a pixel). A game recorded with `--data-use-tiles` says
all four. A byte that reaches the tile area of video memory as it is lands
at some address there, and:

- the low four bits of the address are where in a tile it is, so the byte
  that lands on a multiple of 16 is the first of a tile;
- the byte after it in the ROM lands at the next address if the tiles are 2
  bits a pixel, and two addresses on if they are 1 bit a pixel, where the
  game writes each byte twice or leaves the other half of every row alone;
- a copy that begins with a byte begins a picture, unless the game worked
  out where to read it from something else in the ROM, as it does for the
  second half of a sheet it was told the start of. Then it goes on with the
  picture before, if that is where the picture before would have put it.

So the record has the bytes in runs (`tile_runs`): a byte a copy began with
and the bytes that followed on from it, with how far apart they landed
(`step`), where the first landed (`to`) and how the copy came by its address
(`begun`). `uses` adds the records up and writes each region's runs into the
manifest. `tiles DIR` makes pictures of them: a run that goes on with the
picture before it is joined to it, a run stops being a picture at the first
byte the game also used as it is for something else, and from the first
whole tile to the last is one view. Bytes between two runs that no record
shows in any use are left out: they are as likely another picture the game
did not get to as tiles of these.
"""
from __future__ import annotations

import bisect
import hashlib
import gb_data_names

TILE_AREA = (0x8000, 0x9800)
BEGUN = ("told", "worked_out", "unknown")       # the strongest first: what one record says, another may better
RUN_FIELDS = {"offset", "size", "step", "to", "begun"}
MADE = "seen"                   # a view's `from`: `tiles DIR` made it, from what was seen
SCALE = 2                       # the sheet draws each pixel this many times across


class TilesError(Exception):
    pass


def bits(step: int) -> int:
    """Bits a pixel of tiles whose bytes landed `step` addresses apart."""
    return 2 if step == 1 else 1


def starts_tile(step: int, to: int) -> bool:
    """Whether a byte that landed at `to` is the first of its tile. At 1 bit
    a pixel a row of the tile is two addresses, and the byte may be at either."""
    return to % 16 < step


class Landed:
    """What records say of where ROM bytes landed in the tile area. Of each
    byte: how far it landed after the byte before it (0 if it did not follow
    on from it), where it landed, how the copies that began with it came by
    their address (the strongest of BEGUN), and whether it landed at more
    than one place in a tile."""

    def __init__(self) -> None:
        self.bytes: dict[int, list] = {}
        self.records = 0            # records that say where bytes landed
        self.without = 0            # and records made without --data-use-tiles, which do not
        self.order: list[int] = []  # the bytes in the order they lie, once all records are in

    def add(self, record: dict, path) -> None:
        if "tile_runs" not in record:
            self.without += 1
            return
        self.records += 1
        runs = record["tile_runs"]
        if not isinstance(runs, list):
            raise TilesError(f"{path}: its tile_runs are not a list")
        size = record["rom"]["size"]
        for run in runs:
            if not isinstance(run, dict) or type(run.get("offset")) is not int or \
                    type(run.get("size")) is not int or run["offset"] < 0 or run["size"] < 1 or \
                    run["offset"] + run["size"] > size:
                raise TilesError(f"{path}: a run of its tile_runs is not inside the ROM")
            if run.get("mixed") is True:
                for offset in range(run["offset"], run["offset"] + run["size"]):
                    self.bytes.setdefault(offset, [0, 0, len(BEGUN), True])[3] = True
                continue
            step, to, begun = run.get("step"), run.get("to"), run.get("begun")
            if step not in (0, 1, 2) or type(to) is not int or begun not in BEGUN or \
                    (step == 0) != (run["size"] == 1) or not TILE_AREA[0] <= to < TILE_AREA[1]:
                raise TilesError(f"{path}: the tile run at offset {run['offset']:#x} is not one this gb_data.py "
                                 f"reads")
            for index in range(run["size"]):
                landed = to + index * step
                entry = self.bytes.setdefault(run["offset"] + index, [0, landed, len(BEGUN), False])
                if (entry[1] ^ landed) & 15:
                    entry[3] = True             # two runs of the game disagree about where in a tile it is
                entry[1] = min(entry[1], landed)
                if index:
                    if entry[0] not in (0, step):
                        entry[3] = True
                    entry[0] = step
                else:
                    entry[2] = min(entry[2], BEGUN.index(begun))

    def runs(self, region: dict) -> list[dict]:
        """A region's bytes that landed in the tile area, as the runs the
        manifest keeps: placed from the region's start, and otherwise as a
        record has them."""
        known = self.bytes
        if len(self.order) != len(known):
            self.order = sorted(known)
        runs: list[dict] = []
        start, end = region["offset"], region["offset"] + region["size"]
        for offset in self.order[bisect.bisect_left(self.order, start):bisect.bisect_left(self.order, end)]:
            entry = known[offset]
            last = runs[-1] if runs else None
            follows = last is not None and last["offset"] + last["size"] == offset - start
            if entry[3]:
                if follows and last.get("mixed"):
                    last["size"] += 1
                else:
                    runs.append({"offset": offset - start, "size": 1, "mixed": True})
            elif follows and not last.get("mixed") and entry[0] and last["step"] in (0, entry[0]) and entry[2]:
                last["step"] = entry[0]
                last["size"] += 1
            else:
                # A byte a copy was told to begin with begins a run, whatever ran on through it; and so
                # does one that followed on from a byte in another file.
                runs.append({"offset": offset - start, "size": 1, "step": 0, "to": entry[1],
                             "begun": BEGUN[min(entry[2], len(BEGUN) - 1)]})
        return runs

    def into(self, regions: list[dict]) -> list[str]:
        """Write each region's runs into it (`tile_runs`), and say in a line
        or two what was written. Records that say nothing of it leave the
        regions without, and nothing to say."""
        total = count = 0
        for region in regions:
            runs = self.runs(region) if self.records else []
            if runs:
                region["tile_runs"] = runs
                total += sum(run["size"] for run in runs)
                count += len(runs)
            else:
                region.pop("tile_runs", None)
        if not self.records:
            return []
        said = [f"{total:,} bytes landed in the tile area as they are, in {count:,} "
                f"{'run' if count == 1 else 'runs'}: `tiles` with no file named draws the whole tiles of them"]
        if self.without:
            said.append(f"{self.without} of the records {'was' if self.without == 1 else 'were'} made without "
                        f"--data-use-tiles, and {'does' if self.without == 1 else 'do'} not say where")
        return said


def check_runs(region: dict) -> None:
    """The runs the manifest has of a region have to be ones `uses` wrote."""
    runs = region["tile_runs"]
    if not isinstance(runs, list):
        raise TilesError(f"the tile runs of {region['file']} are not a list")
    end = 0
    for run in runs:
        if not isinstance(run, dict) or any(type(run.get(field)) is not int for field in ("offset", "size")) or \
                run["offset"] < end or run["size"] < 1 or run["offset"] + run["size"] > region["size"]:
            raise TilesError(f"the tile runs of {region['file']} are not inside it, one after another")
        end = run["offset"] + run["size"]
        if set(run) == {"offset", "size", "mixed"} and run["mixed"] is True:
            continue
        if set(run) != RUN_FIELDS or run["step"] not in (0, 1, 2) or type(run["to"]) is not int or \
                run["begun"] not in BEGUN or (run["step"] == 0) != (run["size"] == 1):
            raise TilesError(f"a tile run of {region['file']} is not one `uses` wrote")


def unseen(region: dict, start: int, end: int) -> bool:
    """Whether no record shows the bytes from `start` to `end` of a region in any use."""
    return not any(seen["offset"] < end and start < seen["offset"] + seen["size"] for seen in region.get("seen", ()))


def other_use(region: dict, start: int, end: int) -> int:
    """The first byte from `start` to `end` of a region that the game also
    used as it is for something that is no tile, or `end`."""
    for seen in region.get("seen", ()):
        if seen["offset"] < end and start < seen["offset"] + seen["size"] and set(seen["as_it_is"]) - {"tiles"}:
            return max(start, seen["offset"])
    return end


def pictures(region: dict) -> tuple[list[dict], dict[str, int]]:
    """The pictures a region's tile runs make, each as {offset, tiles, bpp},
    and how many bytes went where:

    - `mixed` landed at more than one place in a tile, and are no picture;
    - `alone` followed on from nothing and had nothing follow on from them;
    - `other` are what is left of a run from the first byte the game also
      used as it is for something else: a copy can take in more than tiles;
    - `ragged` are the ends of a picture that are not a whole tile;
    - `gaps` and `gap_bytes` are the stretches between two runs of tiles
      that no record shows in any use. They are left out: what lies between
      two sheets the game drew is as likely a third it did not, and drawn
      with them it would make one picture of three things."""
    count = {"mixed": 0, "alone": 0, "other": 0, "ragged": 0, "gaps": 0, "gap_bytes": 0}
    pieces: list[dict] = []
    before = None                   # where the run of tiles before this one ended
    for run in region.get("tile_runs", ()):
        if run.get("mixed"):
            count["mixed"] += run["size"]
            continue
        if run["step"] == 0:
            count["alone"] += 1
            continue
        if before is not None and before < run["offset"] and unseen(region, before, run["offset"]):
            count["gaps"] += 1
            count["gap_bytes"] += run["offset"] - before
        before = run["offset"] + run["size"]
        size = other_use(region, run["offset"], before) - run["offset"]
        count["other"] += run["size"] - size
        if size == 0:
            continue
        last = pieces[-1] if pieces else None
        # It goes on with the picture before it if it lies right after it, the game worked out where
        # to read it, and its first byte is where that picture would have put it.
        if last is not None and last["offset"] + last["size"] == run["offset"] and run["begun"] == "worked_out" \
                and run["step"] == last["step"] and (last["to"] + last["size"] * last["step"] - run["to"]) % 16 == 0:
            last["size"] += size
        else:
            pieces.append({"offset": run["offset"], "size": size, "step": run["step"], "to": run["to"]})
    found = []
    for piece in pieces:
        step, tile = piece["step"], 8 * bits(piece["step"])
        skip = next((index for index in range(min(piece["size"], tile))
                     if starts_tile(step, piece["to"] + index * step)), piece["size"])
        tiles = (piece["size"] - skip) // tile
        count["ragged"] += piece["size"] - tiles * tile
        if tiles:
            found.append({"offset": piece["offset"] + skip, "tiles": tiles, "bpp": bits(step)})
    return found, count


# ---------------------------------------------------------------------------
# `tiles DIR`: the pictures written, and something to look through


def picture_name(region: dict, offset: int) -> str:
    """The file a picture of a region's tiles from `offset` is, as `tiles DIR FILE` names it."""
    name = region["file"]
    stem = name[:-len(".bin")] if name.endswith(".bin") else name
    return f"{stem}.tiles.png" if offset == 0 else f"{stem}.tiles_{region['address'] + offset:04X}.png"


def shape(view: dict) -> tuple:
    return view["offset"], view["tiles"], view["bpp"], view["width"]


def gives_way(path, view: dict, force: bool) -> bool:
    """Whether a view is a picture `tiles DIR` made that nobody has edited,
    and so makes room for one somebody tells `tiles` of. An edited one does
    only with --force."""
    return view.get("from") == MADE and \
        (force or not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() == view["sha256"])


def plan(tool, directory, region: dict, content: bytes, width: int, force: bool, said: dict[str, list]) \
        -> tuple[list[dict], list[tuple], list]:
    """What `tiles DIR` is to do in one region: the views that stay as they
    are, the pictures to draw as (view, file, bytes), and the files to take
    away. Why a picture is not drawn goes into `said`."""
    wanted, count = pictures(region)
    for key, number in count.items():
        said[key][0] += number
    views = region.get("views", [])
    mine = {view["file"]: view for view in views if view["kind"] == "tiles" and view.get("from") == MADE}
    kept = [view for view in views if view["file"] not in mine]
    taken = [(start, end, "text" if view["kind"] == "text" else "hand", view)
             for view in kept for start, end in tool.spans(view)]
    edited = {name for name, view in mine.items()
              if tool.inside(directory, name).exists() and
              tool.sha256(tool.inside(directory, name).read_bytes()) != view["sha256"]}
    # A picture that is there keeps the file it has, which `names` may have called by a name.
    draw = [{"kind": "tiles",
             "file": gb_data_names.kept(region, "tiles", picture["offset"],
                                        picture_name(region, picture["offset"])),
             "offset": picture["offset"],
             "tiles": picture["tiles"], "width": min(width, picture["tiles"]), "bpp": picture["bpp"]}
            for picture in wanted]
    # A picture of mine that has been edited stays as it is, and keeps its bytes, whatever the runs make now.
    still = {view["file"]: shape(view) for view in draw}
    for name in sorted(edited if not force else ()):
        kept.append(mine[name])
        if still.get(name) == shape(mine[name]):
            said["left"].append(name)
        else:
            taken += [(start, end, "edited", mine[name]) for start, end in tool.spans(mine[name])]
            said["edited"].append(name)
    names = {view["file"] for view in kept}
    new = []
    for view in draw:
        start, end = tool.spans(view)[0]
        if view["file"] in said["left"]:
            continue
        over = next(((why, other) for low, high, why, other in taken if low < end and start < high), None)
        if over:
            why, other = over
            if why == "hand" and shape(other)[:3] == shape(view)[:3]:
                said["told"].append(other["file"])
            elif why != "edited":
                said[why].append(f"{view['tiles']} tiles at {tool.view_at(region, view)}: {other['file']} has "
                                 f"bytes of them")
            continue
        target = tool.inside(directory, view["file"])
        if target.exists() and view["file"] not in mine and not force:
            raise TilesError(f"{view['file']} is already there and the manifest does not know it: --force writes "
                             f"over it")
        new.append((view, target, content[start:end]))
        names.add(view["file"])
    return kept, new, [tool.inside(directory, name) for name in mine if name not in names]


def tiles_nowhere(region: dict) -> int:
    """How many of a region's bytes went to the tile area as they are with no run to say where."""
    covered = bytearray(region["size"])
    for run in region.get("tile_runs", ()):
        covered[run["offset"]:run["offset"] + run["size"]] = b"\x01" * run["size"]
    return sum(seen["size"] - sum(covered[seen["offset"]:seen["offset"] + seen["size"]])
               for seen in region.get("seen", ()) if "tiles" in seen["as_it_is"])


def found(tool, args) -> int:
    """`gb_data.py tiles DIR`: a view for every picture the tile runs make.

    `tool` is what gb_data.py defines: that file is the program, and cannot
    be imported from here. Everything is worked out before anything is
    written. A view somebody told `tiles` of, and the strings of a file of
    text, keep their bytes: a picture that would take any of them is not
    drawn. A picture this made before is drawn again, or taken away if the
    runs no longer make it, unless it has been edited: then it stays as it
    is and keeps its bytes, unless --force is given."""
    directory = args.directory
    if args.offset or args.tiles is not None or args.bpp != 2:
        raise TilesError("--offset, --tiles and --bpp go with a file: with none named, what the game was seen to do "
                         "says where the tiles are")
    if args.width < 1:
        raise TilesError("a row of the picture has at least one tile")
    manifest, loaded = tool.load_tree(directory)
    for region, _ in loaded:
        if "tile_runs" in region:
            check_runs(region)
    if not any("tile_runs" in region for region, _ in loaded):
        raise TilesError("nothing says yet where the game puts its tiles: run it with --data-use FILE and "
                         "--data-use-tiles, and `uses` on what it records")
    said: dict[str, list] = {key: [0] for key in ("mixed", "alone", "other", "ragged", "gaps", "gap_bytes")}
    said.update({key: [] for key in ("told", "hand", "text", "edited", "left")})
    in_code = [(region, run) for region, _ in loaded if region["kind"] != "data"
               for run in region.get("tile_runs", ()) if not run.get("mixed") and run["step"]]
    nowhere = sum(tiles_nowhere(region) for region, _ in loaded if region["kind"] == "data")
    plans = [(region, *plan(tool, directory, region, content, args.width, args.force, said))
             for region, content in loaded if region["kind"] == "data" and
             ("tile_runs" in region or any(view.get("from") == MADE for view in region.get("views", ())))]

    wrote = same = removed = 0
    drawn = {2: [0, 0], 1: [0, 0]}          # by bits a pixel: pictures, tiles
    for region, kept, new, drop in plans:
        for view, target, data in new:
            png = tool.write_png(*tool.draw_tiles(data, view["bpp"], view["width"]))
            if target.exists() and target.read_bytes() == png:
                same += 1
            else:
                target.write_bytes(png)
                wrote += 1
            view.update({"sha256": tool.sha256(png), "data_sha256": tool.sha256(data), "from": MADE})
            kept.append(view)
            drawn[view["bpp"]][0] += 1
            drawn[view["bpp"]][1] += view["tiles"]
        for path in drop:
            if path.exists():
                path.unlink()
            removed += 1
        if kept:
            region["views"] = sorted(kept, key=lambda view: tool.spans(view)[0])
        else:
            region.pop("views", None)
    tool.write_manifest(directory, manifest)

    def number(count: int, one: str, many: str) -> str:
        return f"{count:,} {one if count == 1 else many}"

    tiles = drawn[2][1] + drawn[1][1]
    print(f"{number(wrote + same, 'picture', 'pictures')} of the tiles the game was seen to draw: {tiles:,} tiles, "
          f"{drawn[2][1] * 16 + drawn[1][1] * 8:,} bytes ({wrote} written, {same} there already and the same)")
    for depth in (2, 1):
        if drawn[depth][0]:
            print(f"  {number(drawn[depth][0], 'picture', 'pictures')} at {depth} {'bits' if depth == 2 else 'bit'} "
                  f"a pixel, {drawn[depth][1]:,} tiles")
    if said["left"]:
        print(f"  {number(len(said['left']), 'more has', 'more have')} been edited, and "
              f"{'is' if len(said['left']) == 1 else 'are'} left as "
              f"{'it is' if len(said['left']) == 1 else 'they are'}")
    if removed:
        print(f"  {number(removed, 'picture', 'pictures')} that the runs no longer make "
              f"{'was' if removed == 1 else 'were'} taken away")
    if said["gaps"][0]:
        print(f"  {number(said['gaps'][0], 'stretch', 'stretches')} between two runs of tiles in one file, "
              f"{said['gap_bytes'][0]:,} bytes, {'is' if said['gaps'][0] == 1 else 'are'} in no record in any use, "
              f"and {'is' if said['gaps'][0] == 1 else 'are'} left out: what the game was not seen to draw may be "
              f"another picture")
    if said["told"]:
        print(f"  {number(len(said['told']), 'more is a picture', 'more are pictures')} somebody told `tiles` of "
              f"already, of the same tiles: {', '.join(said['told'][:4])}"
              + (f" and {len(said['told']) - 4} more" if len(said["told"]) > 4 else ""))
    for why, words in (("hand", "a picture somebody told `tiles` of has bytes of each, and stays as it is"),
                       ("text", "a file of text has strings in each"),):
        if said[why]:
            print(f"  {number(len(said[why]), 'run of tiles is', 'runs of tiles are')} not drawn: {words}")
            for line in said[why][:6]:
                print(f"    {line}")
    if said["edited"]:
        print(f"  {number(len(said['edited']), 'picture that has', 'pictures that have')} been edited "
              f"{'is' if len(said['edited']) == 1 else 'are'} not what the runs make any more, and "
              f"{'is' if len(said['edited']) == 1 else 'are'} left with the bytes "
              f"{'it has' if len(said['edited']) == 1 else 'they have'}: --force draws afresh, and the edit is "
              f"lost")
        for line in said["edited"][:6]:
            print(f"    {line}")
    out = sum(said[key][0] for key in ("mixed", "alone", "other", "ragged"))
    if out:
        print(f"  {out:,} bytes that landed in the tile area are in no picture: {said['ragged'][0]:,} at the ends "
              f"of a run that are not a whole tile, {said['other'][0]:,} from the first byte of a run that the "
              f"game also used for something else, {said['mixed'][0]:,} that landed at more than one place in a "
              f"tile, {said['alone'][0]:,} that landed alone")
    if nowhere:
        print(f"  {nowhere:,} bytes went to the tile area in records that do not say where: record with "
              f"--data-use-tiles for pictures of them")
    if in_code:
        files = len({region["file"] for region, _ in in_code})
        print(f"  {sum(run['size'] for _, run in in_code):,} bytes that landed in the tile area as tiles are in "
              f"{number(files, 'file', 'files')} of code, which cannot be drawn: `annotations` writes them for "
              f"gbrecomp to be told they are data, and a tree split from that export has them in data files")
        for region, run in in_code[:6]:
            print(f"    {region['bank']:02X}:{region['address'] + run['offset']:04X} {run['size']:5,d} bytes in "
                  f"{region['file']}")
        if len(in_code) > 6:
            print(f"    and {len(in_code) - 6} more runs")
    look_through(tool, directory, loaded)
    return 0


def look_through(tool, directory, loaded) -> None:
    """Write the sheet and the list of every picture of tiles the tree has."""
    entries, lines = [], []
    for region, _ in loaded:
        for view in region.get("views", ()):
            if view["kind"] != "tiles":
                continue
            label = tool.view_at(region, view)
            wide, high = tool.sheet_size(view)
            lines.append(f"{label}  {view['tiles']:4d} tiles  {view['bpp']} bpp  {wide:3d}x{high:<4d} {view['file']}"
                         + ("" if view.get("from") == MADE else "   told by hand"))
            try:
                size = tool.read_png(view["file"], tool.inside(directory, view["file"]).read_bytes())
            except (OSError, tool.DataError):
                continue                # `status` and `build` say what is wrong with it
            if size[:2] == (wide, high):
                entries.append((label, wide, high, size[2]))
    if not entries:
        return
    head = ["# Every picture of tiles in this tree: where its first tile is in the ROM, how many tiles,",
            "# bits a pixel, its size in pixels, and its file. tiles.png shows them under the same places.", ""]
    sheet, index = directory / "tiles.png", directory / "tiles.txt"
    index.write_text("\n".join(head + lines) + "\n", encoding="utf-8", newline="\n")
    sheet.write_bytes(tool.write_png(*contact_sheet(entries)))
    print(f"wrote {sheet} and {index}: the {len(lines)} pictures of tiles in the tree, to look through")


# Hex digits and a colon, three pixels wide and five high, a row of each in three bits.
DIGITS = {
    "0": (7, 5, 5, 5, 7), "1": (2, 6, 2, 2, 7), "2": (7, 1, 7, 4, 7), "3": (7, 1, 7, 1, 7),
    "4": (5, 5, 7, 1, 1), "5": (7, 4, 7, 1, 7), "6": (7, 4, 7, 5, 7), "7": (7, 1, 1, 1, 1),
    "8": (7, 5, 7, 5, 7), "9": (7, 5, 7, 1, 7), "A": (7, 5, 7, 5, 5), "B": (6, 5, 6, 5, 6),
    "C": (7, 4, 4, 4, 7), "D": (6, 5, 5, 5, 6), "E": (7, 4, 7, 4, 7), "F": (7, 4, 7, 4, 4),
    ":": (0, 2, 0, 2, 0),
}
MARGIN, LABEL_HIGH = 8, 14
SHEET_WIDTH = 4 * (16 * 8 * SCALE + MARGIN) + MARGIN     # four pictures of sixteen tiles across
PAPER, INK = 216, 40


def contact_sheet(entries: list[tuple[str, int, int, bytes]]) -> tuple[int, int, bytes]:
    """One sheet of pictures, each under its label, as its width, height and
    grey pixels. A picture is (label, width, height, grey pixels). As many
    to a row as fit, each pixel drawn SCALE times across."""
    placed = []
    x = y = MARGIN
    row_high = 0
    for label, width, height, grey in entries:
        cell = max(width * SCALE, len(label) * 8)
        if x > MARGIN and x + cell + MARGIN > SHEET_WIDTH:
            x, y, row_high = MARGIN, y + row_high + MARGIN, 0
        placed.append((x, y, label, width, height, grey))
        x += cell + MARGIN
        row_high = max(row_high, LABEL_HIGH + height * SCALE)
    total_w = max(SHEET_WIDTH, max(left + max(width * SCALE, len(label) * 8) + MARGIN
                                   for left, _, label, width, *_ in placed))
    total_h = y + row_high + MARGIN
    sheet = bytearray([PAPER]) * (total_w * total_h)
    for left, top, label, width, height, grey in placed:
        for index, letter in enumerate(label):
            for row, pattern in enumerate(DIGITS.get(letter, (0,) * 5)):
                for column in range(3):
                    if pattern & (4 >> column):
                        for dy in range(2):
                            start = (top + row * 2 + dy) * total_w + left + index * 8 + column * 2
                            sheet[start:start + 2] = bytes((INK, INK))
        for row in range(height):
            line = bytes(level for level in grey[row * width:(row + 1) * width] for _ in range(SCALE))
            for dy in range(SCALE):
                start = (top + LABEL_HIGH + row * SCALE + dy) * total_w + left
                sheet[start:start + len(line)] = line
    return total_w, total_h, bytes(sheet)
