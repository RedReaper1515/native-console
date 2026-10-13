#!/usr/bin/env python3
"""Decode the checklist ROM's results from a --dump-state file."""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path


def read_state(path: Path) -> dict[str, bytes]:
    data = path.read_bytes()
    blocks, off = {}, 0
    while off + 8 <= len(data):
        tag = data[off:off + 4].decode("ascii")
        size = struct.unpack("<I", data[off + 4:off + 8])[0]
        blocks[tag] = data[off + 8:off + 8 + size]
        off += 8 + size
    return blocks


def results(state: Path, meta: Path) -> tuple[bool, list[tuple[dict, str]]]:
    wram = read_state(state)["WRAM"]
    tests = json.loads(meta.read_text())["tests"]
    done = wram[0x2100] == 0xD0 and wram[0x2101] == len(tests)
    out = []
    for t in tests:
        code = wram[0x2000 + t["index"]]
        out.append((t, "pass" if code == 0x01 else "FAIL" if code == 0xFF else "not reached"))
    return done, out


if __name__ == "__main__":
    done, rows = results(Path(sys.argv[1]), Path(sys.argv[2]))
    for t, verdict in rows:
        print(f"{t['index']:2d} {verdict:11s} {','.join(map(str, t['nuances'])):16s} {t['description']}")
    print("all tests ran" if done else "the ROM did not finish")
    sys.exit(0 if done and all(v == "pass" for _, v in rows) else 1)
