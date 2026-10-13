#!/usr/bin/env python3
"""Check that a save state resumes exactly.

Runs a program three ways with the same scripted input and compares the
machine state (--dump-state) and the picture (--dump-frame) at the end:

    straight   power on, run to frame END
    saved      power on, save a state at frame SAVE, run on to END
    resumed    load that state, run to END

All three must be byte-identical. When the executable carries a recompiled
program (a generated game rather than nes_run), the check is crossed as the
playbook asks: a state saved by recompiled code is resumed by the
interpreter and the other way round, and all of them must agree.

    python tools/nes/state_resume.py --exe build/bin/nes_run --rom game.nes \
        --save-frame 60 --frames 200 --out build/state_resume -- --keys 0:A:90
    python tools/nes/state_resume.py --exe output/game/build/game --recompiled \
        --save-frame 3000 --frames 6000 --out build/state_resume -- --auto-input

Without --rom and --exe pointing at nes_run, the repository's checklist ROM
is assembled and used. Fails closed: a missing file, a run that fails, or
any difference is an error.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def run(command: list[str]) -> None:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        raise SystemExit(f"FAIL: {' '.join(command)}\n{result.stdout}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exe", type=Path, required=True, help="nes_run or a generated game executable")
    parser.add_argument("--rom", type=Path, help="ROM for nes_run (default: the checklist ROM)")
    parser.add_argument("--recompiled", action="store_true", help="the executable carries a recompiled program")
    parser.add_argument("--save-frame", type=int, default=60)
    parser.add_argument("--frames", type=int, default=200)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("extra", nargs="*", help="runner arguments after --, such as --keys")
    args = parser.parse_args()
    if not 0 < args.save_frame < args.frames:
        raise SystemExit("FAIL: need 0 < --save-frame < --frames")
    args.out.mkdir(parents=True, exist_ok=True)

    extra = list(args.extra)
    rom: list[str] = []
    if args.rom:
        rom = [str(args.rom)]
    elif not args.recompiled:
        built = args.out / "checklist.nes"
        run([sys.executable, str(HERE / "checklist" / "make_checklist_rom.py"), "-o", str(built)])
        rom = [str(built)]
        if not extra:
            extra = ["--keys", "0:A+SELECT+UP+LEFT:90", "--keys", "90:B+START+DOWN+RIGHT:100000"]

    modes = {"native": [], "interp": ["--interpreter"]} if args.recompiled else {"interp": []}
    base = [str(args.exe), *rom, "--quiet", *extra]

    def outputs(name: str) -> tuple[Path, Path]:
        return args.out / f"{name}.state", args.out / f"{name}.ppm"

    def finish(name: str, mode: list[str], more: list[str]) -> tuple[bytes, bytes]:
        state, frame = outputs(name)
        for path in (state, frame):
            path.unlink(missing_ok=True)
        run([*base, *mode, "--frames", str(args.frames), "--dump-state", str(state), "--dump-frame", str(frame), *more])
        return state.read_bytes(), frame.read_bytes()

    results: dict[str, tuple[bytes, bytes]] = {}
    for mode_name, mode in modes.items():
        results[f"straight-{mode_name}"] = finish(f"straight-{mode_name}", mode, [])
        saved = args.out / f"at{args.save_frame}-{mode_name}.sav"
        saved.unlink(missing_ok=True)
        results[f"saved-{mode_name}"] = finish(
            f"saved-{mode_name}", mode, ["--save-state", str(saved), "--save-state-frame", str(args.save_frame)])
        if not saved.is_file() or saved.stat().st_size == 0:
            raise SystemExit(f"FAIL: no state written by the {mode_name} run")
        # Resume the state in every mode: the same one, and crossed.
        for resume_name, resume in modes.items():
            name = f"resumed-{mode_name}-in-{resume_name}"
            results[name] = finish(name, resume, ["--load-state", str(saved)])

    reference_name = next(iter(results))
    reference = results[reference_name]
    if len(reference[0]) == 0 or len(reference[1]) == 0:
        raise SystemExit("FAIL: empty state or frame dump")
    failed = False
    for name, got in results.items():
        same_state, same_frame = got[0] == reference[0], got[1] == reference[1]
        print(f"{'ok  ' if same_state and same_frame else 'FAIL'} {name}"
              f"{'' if same_state else '  (state differs)'}{'' if same_frame else '  (frame differs)'}")
        failed = failed or not (same_state and same_frame)
    if failed:
        print(f"FAIL: runs differ from {reference_name}")
        return 1
    print(f"PASS: {len(results)} runs end in the same state and frame "
          f"(saved at frame {args.save_frame}, compared at frame {args.frames})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
