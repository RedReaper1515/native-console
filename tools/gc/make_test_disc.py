#!/usr/bin/env python3
"""Build a small GameCube disc image to test the drive against.

A retail disc is 1.36 GB and nobody's is in this repository. The DVD
interface still has to be tested, so this writes a real one: the same
header, the same filesystem table, the same 32-byte alignment the drive
reads in - just three files in two directories instead of four thousand.

    python tools/gc/make_test_disc.py --out build/gc-programs/test.gcm

tests/gc/programs/gc_disctest.c reads it back and checks every byte, so the
contents here and the expectations there are one pair. The pattern is
generated rather than stored for the same reason: two copies of a literal
drift, an arithmetic rule does not.

The image is a few kilobytes and is built on demand rather than committed,
because a checked-in binary is a thing nobody can review.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_OUT = REPO_ROOT / "build" / "gc-programs" / "test.gcm"

DISC_MAGIC = 0xC2339F3D
HEADER_SIZE = 0x440
ALIGN = 32  # the drive reads in 32-byte units, so everything sits on one

GAME_CODE = b"GCTE01"
GAME_NAME = b"native console disc test"

# The files, in the order the filesystem lists them: a directory's entries
# follow it, which is what makes a filesystem table walkable in one pass.
HELLO = b"Hello from the disc.\n"


def pattern(length: int) -> bytes:
    """The same rule gc_disctest.c checks against."""
    return bytes((i * 7 + 13) & 0xFF for i in range(length))


FILES = [
    ("data/pattern.bin", pattern(2048)),
    ("data/second.bin", bytes([0xA5]) * 32),
    ("hello.txt", HELLO + bytes(ALIGN - len(HELLO) % ALIGN)),
]


def align_up(value: int) -> int:
    return (value + ALIGN - 1) // ALIGN * ALIGN


def build_filesystem(files):
    """(entries, path -> index, index -> name offset, the string table).

    The entries are laid out here; their file offsets are filled in by the
    caller, which is the only thing that knows where the data went.
    """
    directories: list[str] = []
    for path, _ in files:
        parts = path.split("/")[:-1]
        for depth in range(len(parts)):
            directory = "/".join(parts[: depth + 1])
            if directory not in directories:
                directories.append(directory)

    # Entries in filesystem order: the root, then each directory followed by
    # what is inside it, then the files at the top level.
    entries: list[tuple[str, bool]] = [("", True)]
    for directory in directories:
        entries.append((directory, True))
        for path, _ in files:
            if path.rsplit("/", 1)[0] == directory:
                entries.append((path, False))
    for path, _ in files:
        if "/" not in path:
            entries.append((path, False))

    index_of = {path: i for i, (path, _) in enumerate(entries)}
    strings = bytearray()
    name_offset: dict[int, int] = {}
    for i, (path, _) in enumerate(entries):
        if i == 0:
            name_offset[i] = 0
            continue
        name_offset[i] = len(strings)
        strings += path.rsplit("/", 1)[-1].encode() + b"\0"

    return entries, index_of, name_offset, bytes(strings)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()

    entries, index_of, name_offset, strings = build_filesystem(FILES)
    count = len(entries)
    fst_size = count * 12 + len(strings)

    # Layout: header, then the filesystem, then the file data. A real disc
    # puts the executable in between; nothing here needs one, because
    # gc_run loads the program itself.
    fst_offset = align_up(HEADER_SIZE)
    data_at = align_up(fst_offset + fst_size)

    placed: dict[str, tuple[int, int]] = {}
    blob = bytearray()
    for path, content in FILES:
        offset = data_at + len(blob)
        placed[path] = (offset, len(content))
        blob += content
        blob += bytes(align_up(len(blob)) - len(blob))

    table = bytearray()
    for i, (path, is_directory) in enumerate(entries):
        kind = 1 if is_directory else 0
        name = name_offset[i]
        if i == 0:
            table += struct.pack(">BBHII", kind, name >> 16, name & 0xFFFF, 0, count)
        elif is_directory:
            parent = index_of[path.rsplit("/", 1)[0]] if "/" in path else 0
            # A directory's "next" is one past its last descendant.
            last = max(j for j, (other, _) in enumerate(entries)
                       if other == path or other.startswith(path + "/"))
            table += struct.pack(">BBHII", kind, name >> 16, name & 0xFFFF, parent, last + 1)
        else:
            offset, size = placed[path]
            table += struct.pack(">BBHII", kind, name >> 16, name & 0xFFFF, offset, size)
    table += strings

    image = bytearray(data_at + len(blob))
    image[0:6] = GAME_CODE
    struct.pack_into(">I", image, 0x1C, DISC_MAGIC)
    image[0x20:0x20 + len(GAME_NAME)] = GAME_NAME
    struct.pack_into(">I", image, 0x420, 0)          # no executable on the disc
    struct.pack_into(">I", image, 0x424, fst_offset)
    struct.pack_into(">I", image, 0x428, fst_size)
    struct.pack_into(">I", image, 0x42C, fst_size)   # no other disc to share it with
    image[fst_offset:fst_offset + len(table)] = table
    image[data_at:data_at + len(blob)] = blob

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(bytes(image))
    print(f"make_test_disc: {args.out} ({len(image)} bytes, {len(FILES)} files)")
    for path, content in FILES:
        offset, size = placed[path]
        print(f"  {path:<20} at {offset:#08x}, {size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
