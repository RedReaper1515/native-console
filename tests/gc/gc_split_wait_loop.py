#!/usr/bin/env python3
"""A wait loop that roots cut into blocks is still counted rather than run,
and a trace's addresses inside a block no longer cut it.

An interrupt returns into the middle of a wait loop, so a trace from before
resumed entries were left out of traces names the loop's second and third
instructions. gcrecomp leaves out traced addresses inside a block static
analysis finds on its own (recompilers/recompiler-gc/src/main.cpp), so the loop stays one
block. A branch from outside into the middle of a loop still cuts it into
blocks, and the emitter has to recognise the loop across them
(idle_loop_end in recompilers/recompiler-gc/src/emitter.cpp): Shrek 2 spent 28 % of a run
in one such loop.

Program A is `lis r3, 0x8000; loop: lwz r0, 0x3000(r3); cmplwi r0, 0;
beq loop; b .` (the last a wait loop of its own), run with no trace and with
a trace naming loop + 4 and loop + 8: in both the loop is one block, emitted as
a wait loop, with no blocks inside it. Program B enters the same loop at its
second instruction with `b loop + 4`: the block at the head has to be the
whole wait loop, and the block inside it has to exist too.
"""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

TEXT_ADDRESS = 0x80003100

PROGRAM_A = (
    0x3C608000,  # lis    r3, 0x8000
    0x80033000,  # loop:  lwz r0, 0x3000(r3)
    0x28000000,  #        cmplwi r0, 0
    0x4182FFF8,  #        beq loop
    0x48000000,  #        b .
)
LOOP_A = TEXT_ADDRESS + 4

PROGRAM_B = (
    0x3C608000,  # lis    r3, 0x8000
    0x48000008,  #        b loop + 4
    0x80033000,  # loop:  lwz r0, 0x3000(r3)
    0x28000000,  #        cmplwi r0, 0
    0x4182FFF8,  #        beq loop
    0x48000000,  #        b .
)
LOOP_B = TEXT_ADDRESS + 8


def dol(program) -> bytes:
    text = b"".join(struct.pack(">I", word) for word in program)
    header = bytearray(0x100)
    struct.pack_into(">I", header, 0x00, 0x100)  # text 0: file offset
    struct.pack_into(">I", header, 0x48, TEXT_ADDRESS)  # text 0: address
    struct.pack_into(">I", header, 0x90, len(text))  # text 0: size
    struct.pack_into(">I", header, 0xE0, TEXT_ADDRESS)  # entry point
    return bytes(header) + text


def generated(out: Path) -> str:
    return "".join(path.read_text() for path in sorted(out.glob("recompiled_*.c")))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gcrecomp", type=Path, required=True)
    args = parser.parse_args()

    failures = []
    with tempfile.TemporaryDirectory() as scratch:
        root = Path(scratch)
        trace = root / "wait.trace"
        trace.write_text(f"# gcrecomp trace\n{LOOP_A + 4:08x}\n{LOOP_A + 8:08x}\n")

        cases = (
            ("A, no trace", PROGRAM_A, LOOP_A, [], 0, (), (LOOP_A + 4, LOOP_A + 8)),
            ("A, trace inside the loop", PROGRAM_A, LOOP_A, ["--trace", str(trace)], 0, (), (LOOP_A + 4, LOOP_A + 8)),
            ("B, branch into the loop", PROGRAM_B, LOOP_B, [], 1, (LOOP_B + 4,), (LOOP_B + 8,)),
        )
        for index, (label, program, loop, extra, across, present, absent) in enumerate(cases):
            image = root / f"wait{index}.dol"
            image.write_bytes(dol(program))
            out = root / f"out{index}"
            made = subprocess.run([str(args.gcrecomp), str(image), "-o", str(out), "--name", "WAIT"] + extra,
                                  capture_output=True, text=True)
            if made.returncode != 0:
                failures.append(f"{label}: gcrecomp failed\n{made.stdout}{made.stderr}")
                continue
            text = generated(out)

            # A block's definition (it returns the next block to run).
            def defined(address: int) -> bool:
                return re.search(rf"^void \*gc_block_{address:08x}\(GcMachine \*m\) {{", text, re.M) is not None
            counted = re.search(r"(\d+) wait loops counted rather than run \((\d+) of them across blocks\)",
                                made.stderr)
            if not counted or counted.groups() != ("2", str(across)):
                failures.append(f"{label}: expected 2 wait loops (the loop and `b .`), {across} across blocks; "
                                f"gcrecomp said\n{made.stderr}")
            if extra and "2 of them inside a block and left out" not in made.stdout:
                failures.append(f"{label}: expected both traced addresses left out; gcrecomp said\n{made.stdout}")
            head = f"{{0x{loop:08x}u, 0x{loop + 12:08x}u, gc_block_{loop:08x}}}"
            if head not in text or not defined(loop):
                failures.append(f"{label}: the table has no block {head}")
            if f"GC_IDLE_SKIP(0x{loop:08x}u, 3u);" not in text:
                failures.append(f"{label}: the loop's head is not emitted as a wait loop")

            for interior in present:
                if not defined(interior):
                    failures.append(f"{label}: block {interior:08x} missing")
            for interior in absent:
                if defined(interior):
                    failures.append(f"{label}: block {interior:08x} unexpected")

    for failure in failures:
        print(failure)
    print("gc_split_wait_loop: " + ("FAIL" if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
