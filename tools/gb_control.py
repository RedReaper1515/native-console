#!/usr/bin/env python3
"""A generated Game Boy game played by a program: the program's end.

A game started with `--control PORT` connects to the program that listens
there and stops at the start of every frame. Stopped, it says what its
memory holds; told which buttons are held and for how many frames, it runs
on (`runtime/include/gbrt_control.h` has what goes over the connection,
`docs/RUNTIME.md` the option). This module starts the game and is that program's
side of it:

    from gb_control import Game

    with Game(["output/game/build/game", "--headless", "--save-dir", scratch],
              watch=[(0xC000, 0x2000), (0xFF80, 0x7F)]) as game:
        while game.byte(0xFF8B) == 0:       # until the game sets a byte
            game.step("A")                  # a frame with A held
            game.step()                     # and one with nothing held
        game.save_state(scratch / "there.state")

What is held is buttons like a player's: with `--record-run DIR` among the
game's options the run is recorded as it is played and replays afterwards
with no program there (`docs/RUN_RECORDS.md`). While a run is recorded or
replayed the game refuses to have a byte written or a state loaded, since
neither would be in the record.

As a command it plays a few frames and prints what it was asked to read:

    python3 tools/gb_control.py GAME --frames 600 --hold S --read FF80:16 -- --headless --save-dir DIR
"""
from __future__ import annotations

import argparse
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

VERSION = 1
# The letters `--input` and a run's record use, and the bit each button has
# in what the game is told is held.
BUTTONS = {"A": 1, "B": 2, "T": 4, "S": 8, "R": 16, "L": 32, "U": 64, "D": 128}


class ControlError(Exception):
    """The game is not there to be played: it never connected, or is gone."""


class Refused(ControlError):
    """The game would not do what it was asked, and says why."""


def held(buttons: int | str) -> int:
    """`buttons` as the byte the game takes: a number, or letters of BUTTONS."""
    if isinstance(buttons, int):
        if not 0 <= buttons <= 0xFF:
            raise ValueError(f"buttons are one byte, not {buttons}")
        return buttons
    byte = 0
    for letter in buttons.upper():
        if letter in "-. ,":
            continue
        if letter not in BUTTONS:
            raise ValueError(f"{letter!r} is no button; they are {' '.join(BUTTONS)} (T Select, S Start)")
        byte |= BUTTONS[letter]
    return byte


class Game:
    """A game started with `--control`, stopped at the start of a frame.

    `command` is the game and its options; `--control PORT` is added.
    `watch` is the memory every stop is to bring along, as (address,
    length) pairs: `byte()` and `bytes_at()` answer from it without asking
    the game. `log` is a file for what the game prints (nowhere, if None).
    """

    def __init__(self, command: list, watch: list[tuple[int, int]] | tuple = (), log: Path | str | None = None,
                 timeout: float = 30.0, cwd: Path | str | None = None):
        self.frame = 0
        self.cycle = 0
        self.recording = False
        self.replaying = False
        self.closed = False
        self.exit_code: int | None = None
        self.memory = b""
        self._watch: list[tuple[int, int, int]] = []
        self._connection: socket.socket | None = None
        self._log = open(log, "wb") if log is not None else None
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(0.1)
            port = listener.getsockname()[1]
            out = self._log if self._log is not None else subprocess.DEVNULL
            self.process = subprocess.Popen([str(word) for word in command] + ["--control", str(port)],
                                            stdin=subprocess.DEVNULL, stdout=out, stderr=out, cwd=cwd)
            deadline = time.monotonic() + timeout
            while self._connection is None:
                try:
                    self._connection, _ = listener.accept()
                except socket.timeout:
                    if self.process.poll() is not None:
                        raise ControlError(f"the game exited with {self.process.returncode} before it connected"
                                           + self._said()) from None
                    if time.monotonic() > deadline:
                        self.process.kill()
                        raise ControlError(f"the game did not connect in {timeout:g} s") from None
        except BaseException:
            self._drop()
            raise
        finally:
            listener.close()
        self._connection.settimeout(None)
        self._connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        letter, body = self._message()
        if letter != b"H" or len(body) < 6 or struct.unpack_from("<I", body)[0] != VERSION or body[4:6] != b"gb":
            self._drop()
            raise ControlError(f"not a Game Boy game speaking version {VERSION}: {letter!r} {body[:16]!r}")
        self._stopped(*self._message())
        if watch:
            self.watch(watch)

    def __enter__(self) -> "Game":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    # ---- the connection ---------------------------------------------------

    def _said(self) -> str:
        """Where to read what the game printed, for an error's text."""
        return f" (its output is in {self._log.name})" if self._log is not None else ""

    def _drop(self) -> None:
        if self._connection is not None:
            self._connection.close()
            self._connection = None
        if self._log is not None:
            self._log.close()

    def _gone(self, why: str) -> ControlError:
        self.closed = True
        try:
            self.exit_code = self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.exit_code = self.process.wait()
        said = self._said()
        self._drop()
        return ControlError(f"{why} (the game exited with {self.exit_code}){said}")

    def _receive(self, length: int) -> bytes:
        parts, left = [], length
        while left:
            try:
                part = self._connection.recv(min(left, 1 << 20))
            except OSError:
                part = b""
            if not part:
                raise self._gone("the game went away")
            parts.append(part)
            left -= len(part)
        return b"".join(parts)

    def _message(self) -> tuple[bytes, bytes]:
        head = self._receive(5)
        return head[:1], self._receive(struct.unpack_from("<I", head, 1)[0])

    def _send(self, letter: bytes, body: bytes = b"") -> None:
        if self.closed or self._connection is None:
            raise ControlError("the game is closed")
        try:
            self._connection.sendall(letter + struct.pack("<I", len(body)) + body)
        except OSError:
            raise self._gone("the game went away") from None

    def _stopped(self, letter: bytes, body: bytes) -> bool:
        """Take in an F (True) or an X (False: the game is closing)."""
        if letter == b"X":
            self.frame, self.cycle = struct.unpack_from("<QQ", body)
            self.closed = True
            self.exit_code = self.process.wait()
            self._drop()
            return False
        if letter != b"F":
            raise ControlError(f"the game answered {letter!r} where it was to stop")
        self.frame, self.cycle, flags = struct.unpack_from("<QQB", body)
        self.recording, self.replaying = bool(flags & 1), bool(flags & 2)
        self.memory = body[17:]
        return True

    def _done(self) -> None:
        letter, body = self._message()
        if letter == b"E":
            raise Refused(body.decode("utf-8", "replace"))
        if letter != b"K":
            raise ControlError(f"the game answered {letter!r}")

    # ---- what a program does with a stopped game --------------------------

    def watch(self, ranges: list[tuple[int, int]] | tuple) -> None:
        """Have every stop bring these (address, length) stretches along."""
        self._send(b"w", b"".join(struct.pack("<HH", address, length) for address, length in ranges))
        self._done()
        self._watch, at = [], 0
        for address, length in ranges:
            self._watch.append((address, length, at))
            at += length
        self.refresh()

    def refresh(self) -> None:
        """Read the watched memory again, as after a write or a state loaded."""
        self._send(b"q")
        self._stopped(*self._message())

    def read(self, address: int, length: int = 1) -> bytes:
        """`length` bytes from `address`, asked of the game."""
        self._send(b"r", struct.pack("<HI", address, length))
        letter, body = self._message()
        if letter == b"E":
            raise Refused(body.decode("utf-8", "replace"))
        if letter != b"M":
            raise ControlError(f"the game answered {letter!r}")
        return body

    def bytes_at(self, address: int, length: int) -> bytes:
        """`length` bytes from `address`: from what is watched where that
        holds them all, and asked of the game otherwise."""
        for start, size, at in self._watch:
            if start <= address and address + length <= start + size:
                offset = at + address - start
                return self.memory[offset:offset + length]
        return self.read(address, length)

    def byte(self, address: int) -> int:
        for start, size, at in self._watch:
            if start <= address < start + size:
                return self.memory[at + address - start]
        return self.read(address, 1)[0]

    def write(self, address: int, data: bytes | int) -> None:
        """Write into RAM. Refused while a run is recorded or replayed."""
        if isinstance(data, int):
            data = bytes([data])
        self._send(b"p", struct.pack("<H", address) + bytes(data))
        self._done()
        if self._watch:
            self.refresh()

    def save_state(self, path: Path | str) -> None:
        self._send(b"s", str(path).encode("utf-8"))
        self._done()

    def load_state(self, path: Path | str) -> None:
        """Load a save state. Refused while a run is recorded or replayed."""
        self._send(b"l", str(path).encode("utf-8"))
        self._done()
        self.refresh()

    def step(self, buttons: int | str = 0, frames: int = 1) -> bool:
        """Run `frames` frames with `buttons` held, then stop again. False
        if the game closed instead (its frame limit, its window, the end of
        a replay)."""
        self._send(b"g", struct.pack("<BI", held(buttons), frames))
        return self._stopped(*self._message())

    def close(self) -> int | None:
        """Close the game as a player would, so that it writes what it
        writes on the way out (a record's last line, a save). Its exit code."""
        if not self.closed:
            try:
                self._send(b"e")
                while self._stopped(*self._message()):
                    pass
            except ControlError:
                pass
        if self.exit_code is None:
            try:
                self.exit_code = self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.exit_code = self.process.wait()
        self.closed = True
        self._drop()
        return self.exit_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     epilog="What follows `--` goes to the game.")
    parser.add_argument("game", type=Path, help="a generated game's executable")
    parser.add_argument("--frames", type=int, default=60, help="frames to play (60)")
    parser.add_argument("--hold", default="", help="buttons held all the while: letters of UDLRAB, S Start, T Select")
    parser.add_argument("--read", action="append", default=[], metavar="ADDR:LEN",
                        help="memory to print when the frames are over, in hexadecimal; may be repeated")
    parser.add_argument("--log", type=Path, help="a file for what the game prints")
    parser.add_argument("options", nargs="*", help="options for the game, after `--`")
    args = parser.parse_args()
    reads = []
    for text in args.read:
        address, _, length = text.partition(":")
        reads.append((int(address, 16), int(length or "1", 16)))
    with Game([args.game, *args.options], log=args.log) as game:
        running = game.step(args.hold, args.frames)
        print(f"frame {game.frame}, cycle {game.cycle}" + ("" if running else ": the game closed"))
        if running:
            for address, length in reads:
                print(f"{address:04X}: {game.read(address, length).hex(' ')}")
        code = game.close()
    return 0 if code == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
