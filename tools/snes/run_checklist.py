#!/usr/bin/env python3
"""Build the checklist ROM and run it in the interpreter (snes_run).

Prints one line per checklist test with the nuance items it covers, and
fails unless every test passed and the ROM finished (fail closed).

    python tools/snes/run_checklist.py --runner build/bin/snes_run --out DIR
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from checklist_results import results  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--frames", default="1500")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rom, meta, state = args.out / "checklist.sfc", args.out / "checklist.json", args.out / "checklist.state"
    make = subprocess.run([sys.executable, str(HERE / "checklist" / "make_checklist_rom.py"), "-o", str(rom), "--json",
                           str(meta)], capture_output=True, text=True)
    if make.returncode:
        print("checklist ROM assembly failed: " + make.stderr)
        return 1
    if state.exists():
        state.unlink()  # a run that writes no state must not inherit an old verdict
    run = subprocess.run([str(args.runner), str(rom), "--frames", args.frames, "--auto-input", "--quiet",
                          "--dump-state", str(state)], capture_output=True, text=True, timeout=900)
    if run.returncode not in (0, 1) or not state.exists():
        print(f"runner failed (exit {run.returncode}): {run.stderr.strip()}")
        return 1
    done, rows = results(state, meta)
    for t, verdict in rows:
        print(f"{t['index']:2d} {verdict:11s} {','.join(map(str, t['nuances'])):16s} {t['description']}")
    ok = done and all(v == "pass" for _, v in rows)
    print(f"\n{sum(v == 'pass' for _, v in rows)}/{len(rows)} checklist tests pass" + ("" if done else ", ROM did not finish"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
