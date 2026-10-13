#!/usr/bin/env python3
"""Write a tiny GameCube executable (a DOL) for the release smoke test.

It counts to 0x10000 in a loop, stores the count at 0x80003000 and then
branches to itself at 0x8000311c for ever. A generated program built from it
has to finish that loop recompiled, agree with the interpreter under
--differential, and end at the branch - which proves the shipped gcrecomp
and runtime-gc produce a working program without a disc or a toolchain.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

TEXT_ADDRESS = 0x80003100
IDLE_ADDRESS = TEXT_ADDRESS + 0x1C  # the branch to itself

PROGRAM = (
    0x38800000,  # li    r4, 0
    0x3CA00001,  # lis   r5, 1
    0x38840001,  # loop: addi r4, r4, 1
    0x7C042800,  # cmpw  r4, r5
    0x4180FFF8,  # blt   loop
    0x3CC08000,  # lis   r6, 0x8000
    0x90863000,  # stw   r4, 0x3000(r6)
    0x48000000,  # idle: b idle
)


def dol() -> bytes:
    text = b"".join(struct.pack(">I", word) for word in PROGRAM)
    header = bytearray(0x100)
    struct.pack_into(">I", header, 0x00, 0x100)  # text 0: file offset
    struct.pack_into(">I", header, 0x48, TEXT_ADDRESS)  # text 0: address
    struct.pack_into(">I", header, 0x90, len(text))  # text 0: size
    struct.pack_into(">I", header, 0xE0, TEXT_ADDRESS)  # entry point
    return bytes(header) + text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_bytes(dol())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
