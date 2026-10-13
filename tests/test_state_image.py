#!/usr/bin/env python3
"""A save state and the ROM image a game is given, end to end.

Builds a small game that reads two bytes of data once, as it starts, and one
byte again and again, and copies them to high RAM, where a state dump shows
them. A ROM image (`--rom-image`) is the game's ROM with those bytes changed.
Then:

- a state the game makes with no image has the header it always had: the hash
  of its ROM, and nothing where the game is named;
- that state loads in the game given an edited image, which says in a line
  that the state is of another image. The bytes read once stay as the state
  had them, and the byte read again is the image's;
- a state made with an image names the game it is of, loads with no image,
  with the same image (with nothing said) and with another, and each time the
  byte read again is the one of the ROM in use;
- a state made with an image before a state named its game goes with that
  image only, as it did;
- an image that is the ROM byte for byte changes nothing: the state made with
  it is the state made without;
- a state of another game is refused, with or without an image on either
  side, and so is one of another size of ROM;
- a recorded run is of one image: a replay goes on from a state of that image
  and refuses the state of another, as it did before.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from fixtures.make_mapper_rom import NINTENDO_LOGO

START = 0x0150
ONCE = 0x0200            # two bytes the game reads as it starts
AGAIN = 0x0210           # one it reads every time round its loop
HEADER = struct.Struct("<IIQIIIIIIIII")     # GBSavestateFileHeader in runtime/src/gbrt.c, without its last field
BUILT_FROM = HEADER.size                    # where that field is: four bytes that used to be padding
OTHER_IMAGE = "was made with another ROM image of this game"
REFUSED = "Savestate ROM mismatch"
FRAMES = 30


def make_rom(title: bytes, once: tuple[int, int], again: int) -> bytes:
    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, START & 0xFF, START >> 8])
    rom[0x104:0x134] = NINTENDO_LOGO
    rom[0x134:0x134 + len(title)] = title
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    code = [0xF3, 0x31, 0xFF, 0xDF,                                   # di / ld sp,$DFFF
            0x21, ONCE & 0xFF, ONCE >> 8,                             # ld hl,Once
            0x2A, 0xE0, 0x80, 0x2A, 0xE0, 0x81,                       # ld a,[hli] / ldh [$80],a, twice
            0xFA, AGAIN & 0xFF, AGAIN >> 8, 0xE0, 0x82,               # Loop: ld a,[Again] / ldh [$82],a
            0x18, 0xF9]                                               # jr Loop
    rom[START:START + len(code)] = bytes(code)
    rom[ONCE:ONCE + 2] = bytes(once)
    rom[AGAIN] = again
    return bytes(rom)


def edited(rom: bytes, once: tuple[int, int] | None, again: int) -> bytes:
    image = bytearray(rom)
    if once:
        image[ONCE:ONCE + 2] = bytes(once)
    image[AGAIN] = again
    return bytes(image)


def rom_hash(rom: bytes) -> int:
    """The hash a save state has of its ROM (gbrt_rom_hash): FNV-1a, 64 bits."""
    value = 1469598103934665603
    for byte in rom:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def game_of(rom: bytes) -> int:
    """And what a state made with an image has of the ROM its game was built from."""
    value = rom_hash(rom)
    return ((value ^ (value >> 32)) & 0xFFFFFFFF) or 1


def header(state: Path) -> tuple[int, int, int]:
    """The hash and the size of the ROM a state was made with, and the game it names."""
    data = state.read_bytes()
    fields = HEADER.unpack_from(data, 0)
    assert fields[0] == 0x56534247, hex(fields[0])
    return fields[2], fields[3], struct.unpack_from("<I", data, BUILT_FROM)[0]


def run(command: list, expect: int = 0, timeout: int = 900) -> str:
    done = subprocess.run([str(word) for word in command], capture_output=True, text=True, timeout=timeout)
    printed = done.stdout + done.stderr
    assert done.returncode == expect, f"exit {done.returncode}, not {expect}, from {command!r}\n{printed[-4000:]}"
    return printed


def build(gbrecomp: Path, rom_path: Path, project: Path, prefix: str, cmake_args: list[str]) -> Path:
    run([gbrecomp, rom_path, "--no-scan", "--pointer-scan", "off", "-o", project, "--output-prefix", prefix])
    run(["cmake", "-G", "Ninja", "-S", project, "-B", project / "build", "-DCMAKE_BUILD_TYPE=Release", *cmake_args])
    run(["ninja", "-C", project / "build"])
    return next(path for path in (project / "build" / f"{prefix}.exe", project / "build" / prefix) if path.exists())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gbrecomp", required=True, type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="gbstate-") as scratch:
        root = Path(scratch)
        rom = make_rom(b"STATE GAME", (0x11, 0x22), 0x33)
        other_rom = make_rom(b"OTHER GAME", (0x11, 0x22), 0x33)
        (root / "game.gb").write_bytes(rom)
        (root / "other.gb").write_bytes(other_rom)
        game = build(args.gbrecomp, root / "game.gb", root / "game", "state_game", args.cmake_arg)
        other_game = build(args.gbrecomp, root / "other.gb", root / "other", "other_game", args.cmake_arg)
        images = {"a": edited(rom, (0xA1, 0xA2), 0xA3), "b": edited(rom, None, 0xB3), "same": rom,
                  "other": edited(other_rom, None, 0xC3)}
        for name, image in images.items():
            (root / f"{name}.gb").write_bytes(image)
        saves = root / "saves"
        saves.mkdir()
        count = 0

        def play(executable: Path, frames: int, *flags, image: str | None = None, load: Path | None = None,
                 expect: int = 0) -> tuple[list[int], Path, str]:
            """Run `frames` frames, from a state or from the start: what was copied, the state at the end, and
            what was said."""
            nonlocal count
            count += 1
            dump, state = root / f"run{count}.json", root / f"run{count}.state"
            command = [executable, "--headless", "--no-audio", "--limit-frames", frames, "--save-dir", saves,
                       "--dump-state", dump, "--save-state-file", state, *flags]
            if image:
                command += ["--rom-image", root / f"{image}.gb"]
            if load:
                command += ["--load-state-file", load]
            said = run(command, expect, timeout=180)
            copied = json.loads(dump.read_text(encoding="utf-8"))["hram_ff80_fffe"][:3] if dump.exists() else []
            return copied, state, said

        def refused(executable: Path, state: Path, image: str | None = None) -> None:
            copied, _, said = play(executable, FRAMES + 10, image=image, load=state, expect=1)
            assert REFUSED in said and OTHER_IMAGE not in said and not copied, said

        # With no image: the state it always made, and the game ends from it as it does from the start.
        copied, plain, said = play(game, FRAMES)
        assert copied == [0x11, 0x22, 0x33] and OTHER_IMAGE not in said, (copied, said)
        assert header(plain) == (rom_hash(rom), len(rom), 0), header(plain)
        copied, resumed, said = play(game, FRAMES + 10, load=plain)
        assert copied == [0x11, 0x22, 0x33] and OTHER_IMAGE not in said, (copied, said)
        assert header(resumed) == header(plain)

        # The image by itself, from the start: every byte is the image's.
        copied, _, said = play(game, FRAMES, image="a")
        assert copied == [0xA1, 0xA2, 0xA3] and "3 bytes differ from the ROM this game was built from" in said, said

        # The state of the ROM, in the game given the image: it loads, and says what it is. What
        # was read once is the state's; what is read again is the image's.
        copied, with_a, said = play(game, FRAMES + 10, image="a", load=plain)
        assert copied == [0x11, 0x22, 0xA3], copied
        assert said.count(OTHER_IMAGE) == 1 and "stays until the game loads the data again" in said, said
        assert str(plain) in next(line for line in said.splitlines() if OTHER_IMAGE in line), said
        # A state made with an image has the image's hash, as before, and now names its game.
        assert header(with_a) == (rom_hash(images["a"]), len(rom), game_of(rom)), header(with_a)
        assert game_of(rom) != game_of(other_rom) and game_of(rom) != 0

        # That state with no image, with the image it was made with, and with another.
        copied, _, said = play(game, FRAMES + 20, load=with_a)
        assert copied == [0x11, 0x22, 0x33] and said.count(OTHER_IMAGE) == 1, (copied, said)
        copied, _, said = play(game, FRAMES + 20, image="a", load=with_a)
        assert copied == [0x11, 0x22, 0xA3] and OTHER_IMAGE not in said, (copied, said)
        copied, with_b, said = play(game, FRAMES + 20, image="b", load=with_a)
        assert copied == [0x11, 0x22, 0xB3] and said.count(OTHER_IMAGE) == 1, (copied, said)
        assert header(with_b) == (rom_hash(images["b"]), len(rom), game_of(rom)), header(with_b)

        # A state made with an image before states named their game: the same file with nothing
        # there. It goes with that image and no other, as it did.
        before = root / "before.state"
        data = bytearray(with_a.read_bytes())
        data[BUILT_FROM:BUILT_FROM + 4] = bytes(4)
        before.write_bytes(data)
        copied, _, said = play(game, FRAMES + 20, image="a", load=before)
        assert copied == [0x11, 0x22, 0xA3] and OTHER_IMAGE not in said, (copied, said)
        refused(game, before, "b")
        refused(game, before)

        # An image that is the ROM is no other image: nothing is said, and the state made with it
        # is the state made without, byte for byte.
        copied, with_same, said = play(game, FRAMES + 10, image="same", load=plain)
        assert copied == [0x11, 0x22, 0x33] and OTHER_IMAGE not in said, (copied, said)
        assert "0 bytes differ" in said and with_same.read_bytes() == resumed.read_bytes()
        copied, _, said = play(game, FRAMES + 20, image="same", load=with_a)
        assert copied == [0x11, 0x22, 0x33] and said.count(OTHER_IMAGE) == 1, (copied, said)

        # Another game's state is refused as it was, whatever images the two were given.
        copied, others, _ = play(other_game, FRAMES)
        assert copied == [0x11, 0x22, 0x33] and header(others) == (rom_hash(other_rom), len(rom), 0)
        refused(game, others)
        refused(game, others, "a")
        refused(other_game, plain)
        refused(other_game, with_a)
        copied, others_image, said = play(other_game, FRAMES + 10, image="other", load=others)
        assert copied == [0x11, 0x22, 0xC3] and said.count(OTHER_IMAGE) == 1, (copied, said)
        assert header(others_image) == (rom_hash(images["other"]), len(rom), game_of(other_rom))
        refused(game, others_image)
        refused(game, others_image, "a")
        # And a state of a ROM of another size, whether it names this game or has this ROM's hash.
        longer = root / "longer.state"
        for state, image in ((with_a, None), (with_a, "b"), (plain, None), (with_a, "a")):
            data = bytearray(state.read_bytes())
            struct.pack_into("<I", data, 16, len(rom) * 2)
            longer.write_bytes(data)
            refused(game, longer, image)

        # A run is of one image. Recorded with the image, it replays with it, and from a state
        # the replay made; the state of the ROM itself, which the game given the image takes
        # when nothing is replayed, is not one a replay goes on from.
        run_dir, states = root / "run", root / "states"
        states.mkdir()
        run([game, "--headless", "--record-run", run_dir, "--rom-image", root / "a.gb", "--limit-frames", 90])
        segment = run_dir / "segment-0001.run"
        said = run([game, "--headless", "--replay-run", segment, "--rom-image", root / "a.gb", "--save-dir", saves,
                    "--run-states", states, "--run-state-frames", FRAMES])
        assert "end=matched" in said, said
        made = states / f"segment-0001-f{FRAMES:09d}.state"
        assert header(made) == (rom_hash(images["a"]), len(rom), game_of(rom)), header(made)
        said = run([game, "--headless", "--replay-run", segment, "--rom-image", root / "a.gb", "--save-dir", saves,
                    "--load-state-file", made])
        assert "end=matched" in said and "from a state" in said and OTHER_IMAGE not in said, said
        said = run([game, "--headless", "--replay-run", segment, "--rom-image", root / "a.gb", "--save-dir", saves,
                    "--load-state-file", plain], expect=1)
        assert REFUSED in said and OTHER_IMAGE not in said and "replaying" not in said, said
        said = run([game, "--headless", "--replay-run", segment, "--rom-image", root / "a.gb", "--save-dir", saves,
                    "--load-state-file", with_b], expect=1)
        assert REFUSED in said and OTHER_IMAGE not in said and "replaying" not in said, said

    print("save states and ROM images end to end: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
