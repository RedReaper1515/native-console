#!/usr/bin/env python3
"""Run a Wii game under Dolphin with scripted button presses, and bring back its memory, screenshots and sound.

A retail game that goes wrong here goes wrong for a reason nobody can read
off the screen. A second machine running the same disc can be asked what it
has in the same structures at the same point: where the game's heaps begin
and end, what a camera holds, which values are already NaN and harmless.
That is how a two-megabyte difference in where IOS ends a game's MEM2 was
found (docs/WII-PLAN.md, "What the first retail game found"), and how the
game's own picture of a screen was had to compare with ours.

    python tools/wii/oracle_dolphin_game.py --dolphin D:/Tools/Dolphin/Dolphin.exe game.wbfs \\
        --seconds 172 --press 30:A --press 100:AB --press 136:A --shot 139 --shot 150 \\
        --memory out/dolphin.bin --shots out/ --audio out/dolphin.raw

Dolphin is run as a separate program (GPL: a tool here, never linked or
copied), in a user directory of its own under the system's temporary
directory, so nobody's settings or saves are touched; it has its own keys
for a disc, and the disc is the caller's. What comes back is evidence of
what a second implementation does, not of what the hardware does
(AGENTS.md, evidence layers) - and where the two differ, either may be the
one that is wrong.

How it works, and what to know before running it:

- The emulated remote's A and B are bound to the F13 and F14 keys and the
  screenshot hotkey to F15: keys no keyboard has, pressed here with
  SendInput, so nothing else on the desktop sees a letter typed. Dolphin is
  set to take input in the background. A Dolphin window does open.
- Times are seconds from the start, at the game's own speed. Dolphin models
  the disc's speed, so a game takes longer to load there than here: give a
  press time for the screen to be up, and take a screenshot first to see.
- The pointer is wherever Dolphin leaves an unbound one: the middle of the
  screen, which is where the headless runner here puts it too.
- Memory is read through Dolphin's GDB stub after stopping the game: MEM1,
  and MEM2 with --mem2 (five minutes more). The file has the layout of the
  runner's --dump-memory: MEM1, then MEM2 (zeroes when it was not read).
- Sound (--audio) is what the game gave the audio interface, as Dolphin
  dumps it: sixteen-bit little-endian stereo at the console's own rate, with
  no header, which is the layout of the runner's --dump-audio. Silence is
  kept, but the file begins when the game starts its sound, not when the
  game starts, and under the stub Dolphin runs well below a console's speed
  (400 s of waiting gave 45 s of sound): a time in the file is not a time on
  the clock. tools/wii/compare_audio.py lines two such files up by what is
  in them. Dolphin is stopped by killing it, which can lose the last moment.
- Windows only, as written: the keys are pressed with the Win32 API.
"""

from __future__ import annotations

import argparse
import ctypes
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

PORT = 23947
SCAN = {"A": 0x64, "B": 0x65, "S": 0x66}  # F13, F14, F15 as keyboard scan codes
MEM1_SIZE = 0x01800000
MEM2_SIZE = 0x04000000

DOLPHIN_INI = f"""[Core]
CPUCore = 1
SkipIPL = True
GFXBackend = {{backend}}
EmulationSpeed = 1.000000
DSPHLE = True
EnableCheats = False
WiimoteContinuousScanning = False
[General]
GDBPort = {PORT}
[Input]
BackgroundInput = True
[Interface]
ConfirmStop = False
UsePanicHandlers = False
OnScreenDisplayMessages = False
[Display]
RenderToMain = True
Fullscreen = False
[DSP]
Backend = No Audio
DumpAudio = {{audio}}
DumpAudioSilent = True
[Movie]
DumpAudioSilent = True
[Analytics]
PermissionAsked = True
Enabled = False
"""

WIIMOTE_INI = """[Wiimote1]
Source = 1
Device = DInput/0/Keyboard Mouse
Buttons/A = `F13`
Buttons/B = `F14`
Extension = Nunchuk
[Wiimote2]
Source = 0
[Wiimote3]
Source = 0
[Wiimote4]
Source = 0
"""

HOTKEYS_INI = "[Hotkeys]\nDevice = DInput/0/Keyboard Mouse\nGeneral/Take Screenshot = `F15`\n"


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", ctypes.c_ushort), ("wScan", ctypes.c_ushort), ("dwFlags", ctypes.c_ulong),
                ("time", ctypes.c_ulong), ("dwExtraInfo", ctypes.c_void_p)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]

    _anonymous_ = ("u",)
    _fields_ = [("type", ctypes.c_ulong), ("u", _U)]


def key(scan: int, down: bool) -> None:
    event = INPUT()
    event.type = 1  # a keyboard event
    event.ki = KEYBDINPUT(0, scan, 0x0008 | (0 if down else 0x0002), 0, None)  # by scan code; key up
    ctypes.windll.user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT))


def packet(body: bytes) -> bytes:
    return b"$" + body + b"#" + b"%02x" % (sum(body) & 0xFF)


class Stub:
    """The little of the GDB remote protocol this needs: continue, stop, read memory."""

    def __init__(self, port: int):
        deadline = time.time() + 90
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    raise RuntimeError("Dolphin's GDB stub did not answer; is this build one that has it?")
                time.sleep(0.5)
        self.s.settimeout(60)
        self.buffer = b""

    def receive(self) -> bytes:
        while True:
            start = self.buffer.find(b"$")
            end = self.buffer.find(b"#", start + 1) if start >= 0 else -1
            if start >= 0 and end >= 0 and len(self.buffer) >= end + 3:
                body = self.buffer[start + 1:end]
                self.buffer = self.buffer[end + 3:]
                self.s.sendall(b"+")
                return body
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("Dolphin closed the connection")
            self.buffer += chunk

    def command(self, body: bytes) -> bytes:
        self.s.sendall(packet(body))
        return self.receive()

    def read(self, address: int, length: int) -> bytes:
        out = bytearray()
        while length:
            n = min(length, 0x800)
            reply = self.command(b"m%x,%x" % (address, n))
            # A stop reply or the registers can still be on their way.
            while len(reply) != 2 * n and not (reply.startswith(b"E") and len(reply) == 3):
                reply = self.receive()
            out += bytes(n) if reply.startswith(b"E") else bytes.fromhex(reply.decode("ascii"))
            address += n
            length -= n
        return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", type=Path)
    parser.add_argument("--dolphin", type=Path, required=True)
    parser.add_argument("--seconds", type=float, required=True, help="how long the game runs before it is stopped")
    parser.add_argument("--press", action="append", default=[], metavar="T:KEYS",
                        help="at T seconds hold KEYS (letters of A and B) for a quarter of a second")
    parser.add_argument("--shot", action="append", default=[], type=float, metavar="T",
                        help="take a screenshot at T seconds (needs --shots)")
    parser.add_argument("--shots", type=Path, help="directory the screenshots are copied to")
    parser.add_argument("--memory", type=Path, help="file the game's memory is written to")
    parser.add_argument("--mem2", action="store_true", help="read MEM2 as well as MEM1")
    parser.add_argument("--audio", type=Path, help="file the game's sound is written to, as --dump-audio writes it")
    args = parser.parse_args()

    if sys.platform != "win32":
        print("oracle_dolphin_game: the key presses are Win32's; this runs on Windows only", file=sys.stderr)
        return 1
    if not args.dolphin.exists() or not args.game.exists():
        print("oracle_dolphin_game: no such Dolphin or game", file=sys.stderr)
        return 1
    if (args.dolphin.parent / "portable.txt").exists():
        print(f"oracle_dolphin_game: {args.dolphin.parent / 'portable.txt'} overrides the user directory",
              file=sys.stderr)
        return 1
    if args.shot and not args.shots:
        parser.error("--shot needs --shots DIRECTORY")

    events = [(float(t), keys.upper()) for t, keys in (item.split(":") for item in args.press)]
    events += [(t, "S") for t in args.shot]
    for _, keys in events:
        if any(k not in SCAN for k in keys):
            parser.error("keys are letters of A and B")

    user = Path(tempfile.mkdtemp(prefix="ncwii-dolphin-"))
    (user / "Config").mkdir(parents=True)
    # A picture needs a real video backend; without one the null backend is quicker.
    (user / "Config" / "Dolphin.ini").write_text(
        DOLPHIN_INI.format(backend="OGL" if args.shot else "Null", audio="True" if args.audio else "False"),
        encoding="ascii")
    (user / "Config" / "WiimoteNew.ini").write_text(WIIMOTE_INI, encoding="ascii")
    (user / "Config" / "Hotkeys.ini").write_text(HOTKEYS_INI, encoding="ascii")
    process = subprocess.Popen([str(args.dolphin), "-b", "-u", str(user), "-e", str(args.game)])
    try:
        stub = Stub(PORT)
        stub.s.sendall(packet(b"c"))  # the stub holds the game until told to continue
        started = time.time()

        def press(keys: str) -> None:
            for k in keys:
                key(SCAN[k], True)
            time.sleep(0.25)
            for k in keys:
                key(SCAN[k], False)
            print(f"  {'screenshot' if keys == 'S' else 'pressed ' + keys} at {time.time() - started:.0f} s")

        for at, keys in events:
            threading.Timer(at, press, args=(keys,)).start()
        time.sleep(args.seconds)
        stub.s.sendall(b"\x03")
        print("stopped:", stub.receive()[:40].decode("ascii", "replace"))
        if args.memory:
            began = time.time()
            stub.command(b"g")
            data = stub.read(0x80000000, MEM1_SIZE)
            data += stub.read(0x90000000, MEM2_SIZE) if args.mem2 else bytes(MEM2_SIZE)
            args.memory.parent.mkdir(parents=True, exist_ok=True)
            args.memory.write_bytes(data)
            print(f"memory: {args.memory}, read in {time.time() - began:.0f} s")
        if args.shots:
            args.shots.mkdir(parents=True, exist_ok=True)
            shots = sorted(user.rglob("*.png"))
            for i, shot in enumerate(shots):
                shutil.copy(shot, args.shots / f"dolphin-{i:02d}.png")
            print(f"screenshots: {len(shots)} in {args.shots}")
    finally:
        for scan in SCAN.values():
            key(scan, False)
        process.kill()
        process.wait()
        time.sleep(1)
        if args.audio:
            # Dolphin fills in a WAV's lengths when it stops, and it was not
            # stopped: the samples are everything after the header.
            dumps = sorted(user.rglob("*dspdump*.wav"))
            if dumps:
                data = dumps[-1].read_bytes()[44:]
                args.audio.parent.mkdir(parents=True, exist_ok=True)
                args.audio.write_bytes(data[: len(data) // 4 * 4])
                print(f"audio: {args.audio}, {len(data) // 4} samples")
            else:
                print("oracle_dolphin_game: Dolphin dumped no sound", file=sys.stderr)
        shutil.rmtree(user, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
