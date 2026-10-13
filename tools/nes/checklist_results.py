#!/usr/bin/env python3
"""Decode the NES checklist ROM's results from a --dump-state file.

The state file is a sequence of blocks: a four-character tag, a 32-bit
little-endian length, the data. "RAM " is the 2 KiB of work RAM, "PRAM" the
cartridge's PRG RAM (blargg's protocol at its start: $6000 status,
$6001-$6003 DE B0 61, text from $6004).

    python tools/nes/checklist_results.py checklist.state checklist.json
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

SIGNATURE = bytes([0xDE, 0xB0, 0x61])
RUNNING = 0x80


def read_state(path: Path) -> dict[str, bytes]:
    data = path.read_bytes()
    blocks, off = {}, 0
    while off + 8 <= len(data):
        tag = data[off:off + 4].decode("ascii", "replace")
        size = struct.unpack("<I", data[off + 4:off + 8])[0]
        if off + 8 + size > len(data):
            raise ValueError(f"{path}: block {tag!r} runs past the end of the file")
        blocks[tag] = data[off + 8:off + 8 + size]
        off += 8 + size
    if off != len(data) or "RAM " not in blocks or len(blocks["RAM "]) != 0x800:
        raise ValueError(f"{path}: not a state dump (no 2 KiB RAM block)")
    return blocks


class Results:
    """done: the ROM ran every test and reported. rows: (test, verdict)."""

    def __init__(self, state: Path, meta: Path):
        blocks = read_state(state)
        ram, pram = blocks["RAM "], blocks.get("PRAM", b"")
        info = json.loads(meta.read_text())
        self.tests = info["tests"]
        res = info["results"]
        self.rows = []
        for t in self.tests:
            code = ram[t["ram"]]
            verdict = "pass" if code == res["pass"] else "FAIL" if code == res["fail"] else \
                "not reached" if code == res["not_reached"] else f"bad result ${code:02X}"
            self.rows.append((t, verdict))
        self.signature = pram[1:4] == SIGNATURE
        self.status = pram[0] if self.signature else None
        end = pram.find(b"\0", 4)
        self.text = pram[4:end if end >= 0 else 4].decode("ascii", "replace") if self.signature else ""
        self.count = ram[res["count"]]
        self.done = self.signature and self.status != RUNNING and self.count == len(self.tests)
        self.passed = sum(v == "pass" for _, v in self.rows)
        # Everything must agree: each result byte, the status and the text.
        self.ok = (self.done and self.passed == len(self.rows) and self.status == 0 and self.text == "ALL PASS")

    def summary(self) -> str:
        if not self.signature:
            return "no result: the DE B0 61 signature is not at $6001"
        if not self.done:
            hung = next((t["number"] for t, v in self.rows if v != "pass" and v != "FAIL"), None)
            return f"the ROM did not finish (stopped in test {hung})" if hung else "the ROM did not finish"
        return f"{self.passed}/{len(self.rows)} pass, status ${self.status:02X}, \"{self.text}\""


def results(state: Path, meta: Path) -> tuple[bool, list[tuple[dict, str]]]:
    r = Results(state, meta)
    return r.done, r.rows


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    try:
        r = Results(Path(sys.argv[1]), Path(sys.argv[2]))
    except (OSError, ValueError, KeyError) as error:
        print(f"cannot read the results: {error}")
        return 1
    for t, verdict in r.rows:
        print(f"{t['number']:2d} {verdict:11s} {t['slot']}  {t['description']}")
    print(r.summary())
    return 0 if r.ok else 1


if __name__ == "__main__":
    sys.exit(main())
