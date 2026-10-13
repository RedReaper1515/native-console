#!/usr/bin/env python3
"""Generate the DS runtime's table of save chips from SkyEmu's ROM database.

A DS card's save chip cannot be read from the ROM: the header does not say
which chip the card carries, and the families do not share a protocol. An
emulator has to know it per game. SkyEmu (MIT) ships such a list, adapted
from melonDS's and relicensed by the melonDS team under the MIT license:
src/nds_rom_database.h, one {game code, ROM size, save type} entry a game.

This script converts that file, at a pinned commit and SHA-256, into

    runtime-nds/src/nds_save_db.c

a sorted array of one 32-bit word a game that nds_save_lookup() searches
(runtime-nds/include/nds_save.h describes the packing), with the original
notices in its header comment.

    python tools/nds/gen_save_db.py            # fetch if needed, generate
    python tools/nds/gen_save_db.py --check    # exit 1 if the file on disk is stale
    python tools/nds/gen_save_db.py --source some/copy/of/nds_rom_database.h

The database is kept as roms/nds/skyemu/src/nds_rom_database.h (gitignored;
the place tools/fetch_nds_tests.py would give it) after the first run, so
later runs need no network. --source uses a copy already on disk and writes
nothing but the generated file.

The run fails closed: the source must hash to the pinned value, every entry
must parse, the game codes must be unique, and every save type must be one
the table below knows. The generated file is written only after all of that,
and only if its contents would change.

What the save type numbers mean is SkyEmu's (the NDS_BACKUP_* names in the
same file) with three things to know:

  - Type 8, "NAND 8 MB", is the type of 126 entries, of which two are NAND
    cards (Jam with the Band and its Japanese original). The rest are demo
    and distribution cards and games that have no save at all
    (Electroplankton, for one). For all of them the effect is the right
    one: nothing answers on the save port. They are kept as NAND, as the
    source has them.
  - Three of the type 8 entries are the DSi-enhanced Art Academy, whose
    card GBATEK documents as having an 8 MiB FLASH chip ("FLASH 8192K
    bytes MX25L6445EZNI-10G (Art Academy only, TWL-VAAV)"). CORRECTIONS
    below makes them that.
  - Three entries have the type FFFFFFFFh, unknown. They are left out, so
    that a lookup reports them like any game that is not in the table.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
import urllib.request
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
OUTPUT = REPO_ROOT / "runtime-nds" / "src" / "nds_save_db.c"
DEFAULT_CACHE = REPO_ROOT / "roms" / "nds" / "skyemu"

SOURCE = {
    "repo": "skylersaleh/SkyEmu",
    "commit": "01516d6798e3652b583e6a366085bb51c43b528d",
    "path": "src/nds_rom_database.h",
    "sha256": "a6bdcb9f3253cba2e08ea20c537a5369c81f8706ef7fc2804c5cc2e40b4a6943",
}

KIB = 1024
MIB = 1024 * 1024

# Index = the kind stored in a table word's low four bits. 0-10 are SkyEmu's
# save type numbers; kinds above 10 exist only for CORRECTIONS.
KINDS = [
    ("NDS_SAVE_NONE", 0, "no save"),
    ("NDS_SAVE_EEPROM_SMALL", 512, "EEPROM 0.5 KiB"),
    ("NDS_SAVE_EEPROM", 8 * KIB, "EEPROM 8 KiB"),
    ("NDS_SAVE_EEPROM", 64 * KIB, "EEPROM 64 KiB"),
    ("NDS_SAVE_EEPROM_LARGE", 128 * KIB, "EEPROM 128 KiB"),
    ("NDS_SAVE_FLASH", 256 * KIB, "FLASH 256 KiB"),
    ("NDS_SAVE_FLASH", 512 * KIB, "FLASH 512 KiB"),
    ("NDS_SAVE_FLASH", 1 * MIB, "FLASH 1 MiB"),
    ("NDS_SAVE_NAND", 8 * MIB, "NAND 8 MiB, or no save"),
    ("NDS_SAVE_NAND", 16 * MIB, "NAND 16 MiB"),
    ("NDS_SAVE_NAND", 64 * MIB, "NAND 64 MiB"),
    ("NDS_SAVE_FLASH", 8 * MIB, "FLASH 8 MiB"),
]
SOURCE_KINDS = 11          # SkyEmu's numbers are 0-10
SOURCE_UNKNOWN = 0xFFFFFFFF

# The source's own names for its numbers, checked against the file so that a
# renumbering upstream cannot go unnoticed.
SOURCE_NAMES = {
    "NDS_BACKUP_NONE": 0,
    "NDS_BACKUP_EEPROM_512B": 1,
    "NDS_BACKUP_EEPROM_8KB": 2,
    "NDS_BACKUP_EEPROM_64KB": 3,
    "NDS_BACKUP_EEPROM_128KB": 4,
    "NDS_BACKUP_FLASH_256KB": 5,
    "NDS_BACKUP_FLASH_512KB": 6,
    "NDS_BACKUP_FLASH_1MB": 7,
    "NDS_BACKUP_NAND_8MB": 8,
    "NDS_BACKUP_NAND_16MB": 9,
    "NDS_BACKUP_NAND_64MB": 10,
}

# game code -> (the type the source must have for it, the kind it gets).
# GBATEK, "DS Cartridge Backup": the DSi-enhanced Art Academy card has an
# 8 MiB FLASH chip (ID C2h,20h,17h). VAAV is the code GBATEK names; VAAJ and
# VAAK are the same card's Japanese and Korean releases.
CORRECTIONS = {
    "VAAV": (8, 11),
    "VAAJ": (8, 11),
    "VAAK": (8, 11),
}

# SkyEmu's own license (its LICENSE file at the pinned commit). The database
# file carries the melonDS team's notice, which is copied from the file.
SKYEMU_COPYRIGHT = 'Copyright (c) 2021 Skyler "Sky" Saleh'
DATABASE_COPYRIGHT = "Copyright (c) 2016-2022 melonDS team"

# {game code, ROM size, save type}: the code is always a hex word; one entry
# upstream writes its size as a product and its type by name.
ENTRY = re.compile(r"^\s*\{\s*0x([0-9A-Fa-f]{8})\s*,[^,{}]*,\s*(0x[0-9A-Fa-f]{8}|NDS_BACKUP_\w+)\s*\}\s*,\s*$")
DEFINE = re.compile(r"^#define\s+(NDS_BACKUP_\w+)\s+(\d+)\s*$")
DIGITS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"


class Problem(Exception):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def source_url() -> str:
    return f"https://raw.githubusercontent.com/{SOURCE['repo']}/{SOURCE['commit']}/{SOURCE['path']}"


def load_source(explicit: Path | None, cache: Path) -> bytes:
    """The pinned file's bytes: from --source, from the cache, or fetched."""
    if explicit is not None:
        data = explicit.read_bytes()
        if sha256(data) != SOURCE["sha256"]:
            raise Problem(f"{explicit}: SHA-256 {sha256(data)}, expected {SOURCE['sha256']}")
        return data
    cached = cache / SOURCE["path"]
    if cached.is_file():
        data = cached.read_bytes()
        if sha256(data) == SOURCE["sha256"]:
            return data
    print(f"fetching {source_url()}", flush=True)
    request = urllib.request.Request(source_url(), headers={"User-Agent": "native-console-fetch"})
    with urllib.request.urlopen(request, timeout=300) as response:
        data = response.read()
    if sha256(data) != SOURCE["sha256"]:
        raise Problem(f"the fetched file has SHA-256 {sha256(data)}, expected {SOURCE['sha256']}")
    cached.parent.mkdir(parents=True, exist_ok=True)
    cached.write_bytes(data)
    return data


def notice_of(text: str) -> list[str]:
    """The license comment at the top of the database file, as lines."""
    start = text.find("/*")
    end = text.find("*/", start)
    if start < 0 or end < 0:
        raise Problem("the source has no leading comment to take the notice from")
    lines = [line.strip() for line in text[start + 2:end].strip().splitlines()]
    joined = "\n".join(lines)
    if DATABASE_COPYRIGHT not in joined or "MIT License" not in joined:
        raise Problem("the source's leading comment is not the MIT notice it had at the pinned commit")
    return lines


def parse(text: str) -> list[tuple[str, int]]:
    """[(game code, save type)] in the source's order."""
    names = {}
    entries = []
    for line in text.splitlines():
        define = DEFINE.match(line)
        if define:
            names[define.group(1)] = int(define.group(2))
            continue
        if "{0x" not in line.replace(" ", ""):
            continue
        entry = ENTRY.match(line)
        if not entry:
            raise Problem(f"cannot parse the entry {line.strip()!r}")
        packed = int(entry.group(1), 16)
        # The code is the header's four characters read as a little-endian word.
        code = bytes((packed >> shift) & 0xFF for shift in (0, 8, 16, 24)).decode("latin-1")
        if any(c not in DIGITS for c in code):
            raise Problem(f"game code {code!r} has a character that is not 0-9 or A-Z")
        save_type = entry.group(2)
        if save_type.startswith("0x"):
            entries.append((code, int(save_type, 16)))
        elif save_type in names:
            entries.append((code, names[save_type]))
        else:
            raise Problem(f"{code}: save type {save_type} is not defined above its entry")
    if names != SOURCE_NAMES:
        raise Problem(f"the source's save type numbers are not the ones this script maps: {names}")
    if not entries:
        raise Problem("no entries found")
    codes = [code for code, _ in entries]
    if len(set(codes)) != len(codes):
        raise Problem("a game code appears twice")
    return entries


def base36(code: str) -> int:
    value = 0
    for c in code:
        value = value * 36 + DIGITS.index(c)
    return value


def convert(entries: list[tuple[str, int]]) -> tuple[list[tuple[str, int]], int]:
    """[(game code, kind)] sorted by code, and how many unknown entries were left out."""
    table = []
    unknown = 0
    corrected = set()
    for code, save_type in entries:
        if code in CORRECTIONS:
            expected, kind = CORRECTIONS[code]
            if save_type != expected:
                raise Problem(f"{code}: the source has save type {save_type}, the correction expects {expected}")
            corrected.add(code)
        elif save_type == SOURCE_UNKNOWN:
            unknown += 1
            continue
        elif save_type < SOURCE_KINDS:
            kind = save_type
        else:
            raise Problem(f"{code}: save type {save_type:#x} is not one this script knows")
        table.append((code, kind))
    if corrected != set(CORRECTIONS):
        raise Problem(f"corrections for games the source does not have: {sorted(set(CORRECTIONS) - corrected)}")
    table.sort(key=lambda item: base36(item[0]))
    return table, unknown


def size_text(size: int) -> str:
    return f"0x{size:08X}u"


def render(table: list[tuple[str, int]], unknown: int, source_entries: int, notice: list[str]) -> str:
    counts = Counter(kind for _, kind in table)
    out = [
        "/*",
        " * Generated by tools/nds/gen_save_db.py. Do not edit.",
        " *",
        " * Which save chip each Nintendo DS game's card has, by game code: the",
        " * table behind nds_save_lookup() (see nds_save.h for how a word is packed).",
        " *",
        " * Converted from the ROM database of SkyEmu, MIT license:",
        " *",
        f" *     https://github.com/{SOURCE['repo']}",
        f" *     commit {SOURCE['commit']}",
        f" *     {SOURCE['path']}",
        f" *     (SHA-256 {SOURCE['sha256'][:32]}",
        f" *              {SOURCE['sha256'][32:]})",
        " *",
        f" * {source_entries} entries in the source, {len(table)} here: the {unknown} whose save type the",
        " * source gives as unknown are left out. Taken from each entry: the game code",
        " * and the save type number. Altered: sorted by game code as text, packed",
        " * into one word a game, and the save type of three entries corrected (the",
        " * DSi-enhanced Art Academy, VAAV, VAAJ and VAAK, from \"NAND 8 MB\" to the",
        " * 8 MiB FLASH chip GBATEK documents for that card).",
        " *",
        " * The source's type 8, \"NAND 8 MB\", also covers demo and distribution",
        " * cards and games without a save. Nothing answers on the save port for",
        " * any of them, which NDS_SAVE_NAND gives.",
        " *",
        " * The database file's notice:",
        " *",
    ]
    out += [f" * {line}".rstrip() for line in notice]
    out += [
        " *",
        " * SkyEmu's license is the same MIT text under the notice",
        f" * {SKYEMU_COPYRIGHT}",
        " */",
        '#include "nds_save.h"',
        "",
        "const NdsSaveKind nds_save_db_kinds[] = {",
    ]
    for kind, (name, size, what) in enumerate(KINDS):
        games = counts.get(kind, 0)
        plural = "game" if games == 1 else "games"
        row = f"    {{ {size_text(size)}, {name} }},"
        out.append(f"{row:44}/* {kind:2}: {what}; {games} {plural} */")
    out += [
        "};",
        "const uint32_t nds_save_db_kind_count = sizeof nds_save_db_kinds / sizeof nds_save_db_kinds[0];",
        "",
        "const uint32_t nds_save_db[] = {",
    ]
    words = [f"0x{(base36(code) << 4) | kind:07X}," for code, kind in table]
    for i in range(0, len(words), 8):
        first, last = table[i][0], table[min(i + 7, len(table) - 1)][0]
        out.append("    " + " ".join(words[i:i + 8]) + f" /* {first}-{last} */")
    out += [
        "};",
        "const uint32_t nds_save_db_count = sizeof nds_save_db / sizeof nds_save_db[0];",
        "",
    ]
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path, help="use this copy of nds_rom_database.h (it must match the pin)")
    parser.add_argument("--cache", type=Path, default=DEFAULT_CACHE,
                        help="where a fetched copy is kept (default: roms/nds/skyemu)")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--check", action="store_true", help="write nothing; exit 1 if the output is stale")
    parser.add_argument("--game", action="append", default=[], metavar="CODE",
                        help="print what the table gives for this game code (may repeat)")
    args = parser.parse_args()

    try:
        data = load_source(args.source, args.cache)
        text = data.decode("utf-8")
        entries = parse(text)
        table, unknown = convert(entries)
        generated = render(table, unknown, len(entries), notice_of(text))
    except (Problem, OSError, UnicodeDecodeError) as error:
        print(f"gen_save_db: FAILED: {error}", file=sys.stderr)
        return 1

    counts = Counter(kind for _, kind in table)
    print(f"{SOURCE['repo']}@{SOURCE['commit'][:10]} {SOURCE['path']}: {len(entries)} entries, "
          f"{len(table)} in the table, {unknown} unknown left out")
    for kind, (name, size, what) in enumerate(KINDS):
        print(f"  kind {kind:2}  {counts.get(kind, 0):5}  {what}")
    kinds = dict(table)
    for code in ["AZEE"] + [c.upper() for c in args.game]:
        if code in kinds:
            name, size, what = KINDS[kinds[code]]
            print(f"  {code}: {what} ({name}, {size} bytes)")
        else:
            print(f"  {code}: not in the table")

    current = args.output.read_text(encoding="utf-8") if args.output.is_file() else None
    if current == generated:
        print(f"{args.output}: up to date")
        return 0
    if args.check:
        print(f"{args.output}: stale (run tools/nds/gen_save_db.py)", file=sys.stderr)
        return 1
    args.output.write_text(generated, encoding="utf-8", newline="\n")
    print(f"{args.output}: written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
