#!/usr/bin/env python3
"""Headless, deterministic Nintendo DS reference runs on DeSmuME (py-desmume).

Run it with the oracle environment's interpreter (docs/NDS-PLAN.md, Decisions):

    ..\\tools\\nds-venv\\Scripts\\python.exe tools/nds/oracle_desmume.py ROM --frames N
        [--keys FRAME:BUTTONS[:N]]...   A B X Y L R START SELECT UP DOWN LEFT RIGHT, joined with +
        [--touch FRAME:X,Y[:N]]...      stylus held at bottom-screen pixel X,Y
        [--lid FRAME:N]...              lid closed
        [--dump-frames DIR] [--every K] [--at FRAME]... [--stacked] [--png] [--no-ppm]
        [--hash-frames] [--video-regs] [--first-nonblank]
        [--dump-ram FILE] [--dump-mem ADDR:LEN[:FILE]]... [--regs]
        [--save-dir DIR] [--rtc-time "YYYY-MM-DD HH:MM:SS"] [--quiet] [@FILE]...

--keys, --touch and --lid are nds_run's (runtimes/runtime-nds/src/nds_runner.c): the
input is held while frames FRAME to FRAME+N-1 run, frames counted from 0, N 1
when left out, and it is set before the frame runs. DeSmuME cannot close the
lid for less than 20 frames, so --lid needs its N here. @FILE reads more
options from FILE (several per line, # starts a comment); input scripts are
kept that way.

A capture "at N" (--at N, a multiple of --every, the N of a "frame N" line) is
the state after N frames have been emulated, which is what nds_run --frames N
ends with and what its --dump-frame and --dump-ram write: --at 0 (or "boot")
is the state before any instruction has run, --at 1 is the first rendered
frame, and a key pressed with --keys F is first seen by the game in capture
F+1. (nds_run --hash-frames labels the same picture one lower: its "frame K"
is capture K+1.) DeSmuME ends a frame at the start of scanline 262, the last
line of vertical blank, so that is where every capture is taken; it boots at
the start of line 0, so capture N is 262 + 263 (N - 1) scanlines after boot.

Output, one line per item on stdout (the emulator's own log goes to stderr):

    frame N top=<sha256> bottom=<sha256>                 --hash-frames
    video N dispcnt_a=.. dispcnt_b=.. disp3dcnt=.. powcnt1=.. vramcnt=A,B,C,D,E,F,G,H,I wramcnt=..
            gxstat=.. polygons=.. vertices=..             --video-regs
    regs N arm9 r0=.. ... r15=.. cpsr=.. spsr=.. pc=..    --regs (also arm7)
    mem N ADDR LEN: hex bytes                            --dump-mem without FILE
    dtcm N base=ADDR                                     --dump-ram, when the DTCM hides a mirror
    first_nonblank top=N bottom=N                        --first-nonblank

A hash is the SHA-256 of one screen's 256x192 pixels, three bytes a pixel in
row order: the red, green and blue 5-bit values (0-31). That is every byte of
a PPM's pixel data shifted right by 3, so it does not depend on how a runner
widens 5 bits to 8, and two pictures with the same hash are the same in the
15-bit colour DeSmuME's interface exposes (the hardware's sixth bit, which 3D
and blending produce, is not there to compare). With --dump-frames and
--hash-frames the "frame" and "video" lines are also written to DIR/frames.txt.

Frame files are DIR/NNNNNN-top.ppm and DIR/NNNNNN-bottom.ppm, or with --stacked
one 256x384 DIR/NNNNNN.ppm, the top screen above the bottom one as nds_run
--dump-frame writes it. Each channel is the 5-bit value shifted left by 3.
--png adds a PNG of each; --no-ppm keeps only the PNGs. FILE names may contain
{frame}; when several --at frames share one FILE without it, -NNNNNN is put
before the extension.

Memory is read through the ARM9's bus, so ARM7-only memory (its WRAM at
0x03800000, its I/O) cannot be dumped, and an address inside the DTCM reads the
DTCM. --dump-ram always writes the 4 MiB of main RAM itself: it reads the
range through two mirrors and takes the one the DTCM does not hide, and
reports where the DTCM is so that --dump-mem can fetch it. I/O registers are
read at their natural width (4, 2 or 1 bytes by alignment). The two registers
whose read changes the machine (0x04100000 IPCFIFORECV, 0x04100010 card data)
are refused.

Isolation. DeSmuME's Windows build keeps the battery save (<rom name>.dsv),
cheats (.dct) and anything else relative to the directory of the running
executable, which under py-desmume is the Python installation. This script
points that lookup at --save-dir (default: a fresh temporary directory, removed
afterwards) by replacing GetModuleFileName in the core's import table, opens
the ROM read-only where it is, selects SDL's dummy audio and video drivers
(no window, no sound device) and fails if the core opened any file for writing
outside the save directory. Nothing is read from or written to a DeSmuME
configuration. An existing .dsv in --save-dir is loaded and updated, as on a
cartridge; use an empty directory (or the default) for repeatable runs.

Determinism. The core's real-time clock follows the host clock unless a movie
is being recorded, so the script records one (DIR/oracle.dsm) starting at
--rtc-time (default 2026-01-01 12:00:00, as gba_run): the clock then starts
there and advances with emulated frames. --host-rtc turns that off. The boot is
DeSmuME's direct boot with its built-in BIOS replacement and its own firmware
settings (nickname "DeSmuME", English, birthday 23 June, touch calibration of
16 ADC units to the pixel); no BIOS or firmware image is used, and the
interface has no way to change those settings.

The exit status is non-zero when anything asked for could not be produced. A
hardware exception inside the core (rockwrestler's 64-bit divisions raise one)
ends the run with a message; the captures before it are complete.
"""

from __future__ import annotations

import argparse
import array
import ctypes
import datetime
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

SCREEN_W, SCREEN_H = 256, 192
SCREEN_BYTES = SCREEN_W * SCREEN_H * 3
MAIN_RAM, MAIN_RAM_SIZE = 0x02000000, 0x400000
DTCM_SIZE = 0x4000

# Bits of the keypad word desmume_input_keypad_update() takes.
BUTTONS = {"A": 0, "B": 1, "SELECT": 2, "START": 3, "RIGHT": 4, "LEFT": 5, "UP": 6, "DOWN": 7,
           "R": 8, "L": 9, "X": 10, "Y": 11}
LID_BIT = 14
# The core toggles the lid when it sees the lid key and then ignores the key
# for 30 input polls. This script polls twice per frame.
LID_MIN_FRAMES = 20

DEFAULT_FRAMES = 1  # how long an input is held when no N is given, as in nds_run
DEFAULT_RTC = "2026-01-01 12:00:00"

# An 8-bit channel to its 5-bit value, for hashing.
TO_5_BITS = bytes(value >> 3 for value in range(256))

# Reading these through the bus pops a FIFO or advances a card transfer.
READ_SIDE_EFFECTS = ((0x04100000, 0x04100004), (0x04100010, 0x04100014))


class OracleError(Exception):
    """Something asked for could not be produced."""


# ---------------------------------------------------------------- arguments

def parse_frame(text: str, what: str) -> int:
    if text.lower() == "boot":
        return 0
    try:
        value = int(text, 10)
    except ValueError:
        raise OracleError(f"bad {what} {text!r}: want a frame number") from None
    if value < 0:
        raise OracleError(f"bad {what} {text!r}: frames start at 0")
    return value


def parse_duration(text: str, spec: str) -> int:
    try:
        value = int(text, 10)
    except ValueError:
        value = 0
    if value < 1:
        raise OracleError(f"bad duration in {spec!r}: want a number of frames, 1 or more")
    return value


def parse_keys(spec: str) -> tuple[int, int, int]:
    parts = spec.split(":")
    if len(parts) not in (2, 3):
        raise OracleError(f"bad --keys {spec!r} (want FRAME:BUTTON[+BUTTON...][:N])")
    frame = parse_frame(parts[0], "--keys frame")
    mask = 0
    for name in parts[1].split("+"):
        if name.upper() not in BUTTONS:
            raise OracleError(f"bad --keys {spec!r}: unknown button {name!r} (have {' '.join(BUTTONS)})")
        mask |= 1 << BUTTONS[name.upper()]
    duration = parse_duration(parts[2], spec) if len(parts) == 3 else DEFAULT_FRAMES
    return frame, mask, duration


def parse_touch(spec: str) -> tuple[int, int, int, int]:
    parts = spec.split(":")
    if len(parts) not in (2, 3):
        raise OracleError(f"bad --touch {spec!r} (want FRAME:X,Y[:N])")
    frame = parse_frame(parts[0], "--touch frame")
    try:
        x, y = (int(v, 10) for v in parts[1].split(","))
    except ValueError:
        raise OracleError(f"bad --touch {spec!r} (want FRAME:X,Y[:N])") from None
    if not (0 <= x < SCREEN_W and 0 <= y < SCREEN_H):
        raise OracleError(f"bad --touch {spec!r}: the bottom screen is {SCREEN_W}x{SCREEN_H}")
    duration = parse_duration(parts[2], spec) if len(parts) == 3 else DEFAULT_FRAMES
    return frame, x, y, duration


def parse_lid(spec: str) -> tuple[int, int]:
    parts = spec.split(":")
    if len(parts) not in (1, 2):
        raise OracleError(f"bad --lid {spec!r} (want FRAME:N)")
    frame = parse_frame(parts[0], "--lid frame")
    duration = parse_duration(parts[1], spec) if len(parts) == 2 else DEFAULT_FRAMES
    if duration < LID_MIN_FRAMES:
        raise OracleError(f"--lid {spec}: DeSmuME cannot close the lid for less than {LID_MIN_FRAMES} frames "
                          "(nds_run can; give both the same N)")
    return frame, duration


def parse_mem(spec: str) -> tuple[int, int, str | None]:
    parts = spec.split(":", 2)
    try:
        addr, length = int(parts[0], 0), int(parts[1], 0)
    except (ValueError, IndexError):
        raise OracleError(f"bad --dump-mem {spec!r} (want ADDR:LEN[:FILE], numbers as 0x... or decimal)") from None
    if not (0 <= addr <= 0xFFFFFFFF) or length < 1 or addr + length > 0x100000000:
        raise OracleError(f"bad --dump-mem {spec!r}: outside the 32-bit address space")
    for lo, hi in READ_SIDE_EFFECTS:
        if addr < hi and addr + length > lo:
            raise OracleError(f"--dump-mem {spec!r} covers 0x{lo:08X}, a register whose read changes the machine")
    return addr, length, parts[2] if len(parts) == 3 and parts[2] else None


def parse_rtc(text: str) -> datetime.datetime:
    try:
        return datetime.datetime.strptime(text, "%Y-%m-%d %H:%M:%S")
    except ValueError:
        raise OracleError(f'bad --rtc-time {text!r} (want "YYYY-MM-DD HH:MM:SS")') from None


def lid_pulses(lids: list[tuple[int, int]]) -> set[int]:
    """Frames on which the lid key is pressed for one frame: once to close, once to open."""
    pulses: list[int] = []
    for frame, duration in sorted(lids):
        if pulses and frame - pulses[-1] < LID_MIN_FRAMES:
            raise OracleError(f"--lid {frame}: needs {LID_MIN_FRAMES} frames after the lid last moved")
        pulses += [frame, frame + duration]
    return set(pulses)


# ---------------------------------------------------------------- the core

class Core:
    """libdesmume loaded with its file lookups confined to one directory."""

    def __init__(self, save_dir: Path):
        self.save_dir = save_dir
        self.opened: list[tuple[str, str]] = []   # (path, mode) of every file the core opened
        self.violations: list[str] = []
        self._keep: list[object] = []

        # No window and no sound device: SDL's dummy drivers. Set before SDL loads.
        os.environ["SDL_AUDIODRIVER"] = "dummy"
        os.environ["SDL_VIDEODRIVER"] = "dummy"

        try:
            import desmume
            from desmume.emulator import DeSmuME
        except ImportError as error:
            raise OracleError(f"py-desmume is not importable ({error}); run this with "
                              "..\\tools\\nds-venv\\Scripts\\python.exe") from None
        package = Path(desmume.__file__).resolve().parent
        dll = package / "libdesmume.dll"
        if not dll.is_file():
            raise OracleError(f"{dll} is missing (this script drives the Windows build of py-desmume)")
        os.add_dll_directory(str(package))
        self.lib = ctypes.CDLL(str(dll))
        self._confine()
        # The same module again, initialised (desmume_init) by the wrapper.
        self.emu = DeSmuME(str(dll))
        if self.emu.lib._handle != self.lib._handle:
            raise OracleError("py-desmume loaded a second copy of libdesmume")

        sdl = ctypes.CDLL(str(package / "SDL2.dll"))
        for query in (sdl.SDL_GetCurrentAudioDriver, sdl.SDL_GetCurrentVideoDriver):
            query.restype = ctypes.c_char_p
            if query() != b"dummy":
                raise OracleError(f"SDL did not take its dummy driver (got {query()!r})")

        lib = self.lib
        lib.desmume_memory_read_byte.restype = ctypes.c_uint8
        lib.desmume_memory_read_short.restype = ctypes.c_uint16
        lib.desmume_memory_read_long.restype = ctypes.c_uint32
        for read in (lib.desmume_memory_read_byte, lib.desmume_memory_read_short, lib.desmume_memory_read_long):
            read.argtypes = [ctypes.c_uint32]
        lib.desmume_memory_read_register.restype = ctypes.c_uint32
        lib.desmume_memory_read_register.argtypes = [ctypes.c_char_p]
        lib.desmume_input_keypad_update.argtypes = [ctypes.c_uint16]
        lib.desmume_input_set_touch_pos.argtypes = [ctypes.c_uint16, ctypes.c_uint16]
        lib.desmume_cycle.argtypes = [ctypes.c_int]
        lib.desmume_screenshot.argtypes = [ctypes.c_void_p]
        lib.desmume_running.restype = ctypes.c_int
        self._shot = ctypes.create_string_buffer(SCREEN_BYTES * 2)

    # -- import-table hooks

    def _imports(self) -> dict[str, int]:
        """Imported function name -> address of its slot in the core's import table."""
        base = self.lib._handle

        def u16(a): return ctypes.c_uint16.from_address(a).value
        def u32(a): return ctypes.c_uint32.from_address(a).value
        def u64(a): return ctypes.c_uint64.from_address(a).value

        optional = base + u32(base + 0x3C) + 24
        if u16(optional) != 0x20B:
            raise OracleError("libdesmume.dll is not a 64-bit image")
        slots: dict[str, int] = {}
        descriptor = base + u32(optional + 112 + 8)
        while u32(descriptor + 12):
            names = base + (u32(descriptor) or u32(descriptor + 16))
            table = base + u32(descriptor + 16)
            index = 0
            while True:
                entry = u64(names + 8 * index)
                if not entry:
                    break
                if not entry >> 63:
                    slots[ctypes.string_at(base + (entry & 0x7FFFFFFF) + 2).decode("ascii")] = table + 8 * index
                index += 1
            descriptor += 20
        return slots

    def _hook(self, slots: dict[str, int], name: str, prototype, make, required: bool = False) -> None:
        slot = slots.get(name)
        if slot is None:
            if required:
                raise OracleError(f"libdesmume.dll does not import {name}; its files cannot be confined")
            return
        original = prototype(ctypes.c_void_p.from_address(slot).value)
        replacement = prototype(make(original))
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        protect = kernel32.VirtualProtect
        protect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
        protect.restype = ctypes.c_int
        old = ctypes.c_uint32()
        if not protect(slot, 8, 0x04, ctypes.byref(old)):
            raise OracleError(f"cannot patch the import of {name}")
        ctypes.c_void_p.from_address(slot).value = ctypes.cast(replacement, ctypes.c_void_p).value
        protect(slot, 8, old.value, ctypes.byref(old))
        self._keep += [original, replacement]

    def _note(self, path, mode: str, writes: bool) -> None:
        if isinstance(path, bytes):
            path = path.decode("utf-8", "replace")
        path = path or ""
        self.opened.append((path, mode))
        if not writes:
            return
        inside = False
        try:
            inside = Path(os.path.abspath(path)).is_relative_to(self.save_dir)
        except (OSError, ValueError):
            pass
        if not inside:
            self.violations.append(f"{path} ({mode})")

    def _confine(self) -> None:
        slots = self._imports()
        c, vp, cp, wp, ci, u32 = (ctypes.CFUNCTYPE, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_wchar_p,
                                  ctypes.c_int, ctypes.c_uint32)
        fake_exe = str(self.save_dir / "desmume.exe")

        # PathInfo::LoadModulePath: every DeSmuME path is relative to the executable's directory.
        def module_name_a(original):
            def hook(module, buffer, size):
                if module:
                    return original(module, buffer, size)
                data = fake_exe.encode("mbcs")[:max(size - 1, 0)]
                ctypes.memmove(buffer, data + b"\0", len(data) + 1)
                return len(data)
            return hook

        def module_name_w(original):
            def hook(module, buffer, size):
                if module:
                    return original(module, buffer, size)
                text = fake_exe[:max(size - 1, 0)]
                raw = (text + "\0").encode("utf-16-le")
                ctypes.memmove(buffer, raw, len(raw))
                return len(text)
            return hook

        self._hook(slots, "GetModuleFileNameA", c(u32, vp, vp, u32), module_name_a, required=True)
        self._hook(slots, "GetModuleFileNameW", c(u32, vp, vp, u32), module_name_w)

        # Everything below only watches (or refuses) what the core does to the file system.
        def stream_open(original):
            def hook(path, mode):
                text = mode.decode() if isinstance(mode, bytes) else (mode or "")
                self._note(path, text, any(ch in text for ch in "wa+"))
                return original(path, mode) or 0
            return hook

        def stream_open_s(original):
            def hook(result, path, mode):
                text = (mode or b"").decode()
                self._note(path, text, any(ch in text for ch in "wa+"))
                return original(result, path, mode)
            return hook

        def descriptor_open(original):
            def hook(path, flags, mode):
                self._note(path, f"open 0x{flags:X}", bool(flags & 0x30B))  # WRONLY RDWR APPEND CREAT TRUNC
                return original(path, flags, mode)
            return hook

        def make_directory(original):
            def hook(path, security):
                made = original(path, security)
                if made:
                    self._note(path, "mkdir", True)
                return made
            return hook

        def refuse(what):
            def make(original):
                def hook(*args):
                    self.violations.append(f"{what} {args[-1]!r}")
                    return 1
                return hook
            return make

        def silent_microphone(original):
            def hook(*args):
                return 6  # MMSYSERR_NODRIVER: no recording device is opened
            return hook

        self._hook(slots, "fopen", c(vp, cp, cp), stream_open)
        self._hook(slots, "_wfopen", c(vp, wp, wp), stream_open)
        self._hook(slots, "fopen_s", c(ci, vp, cp, cp), stream_open_s)
        self._hook(slots, "_open", c(ci, cp, ci, ci), descriptor_open)
        self._hook(slots, "_wopen", c(ci, wp, ci, ci), descriptor_open)
        self._hook(slots, "CreateDirectoryW", c(ci, wp, vp), make_directory)
        self._hook(slots, "WritePrivateProfileStringA", c(ci, cp, cp, cp, cp), refuse("WritePrivateProfileString"))
        self._hook(slots, "SetCurrentDirectoryA", c(ci, cp), refuse("SetCurrentDirectory"))
        self._hook(slots, "waveInOpen", c(u32, vp, u32, vp, vp, vp, u32), silent_microphone)

    # -- reading the machine

    def screens(self) -> tuple[bytes, bytes]:
        """(top, bottom) as R,G,B bytes, each channel a 5-bit value << 3."""
        self.lib.desmume_screenshot(self._shot)
        raw = self._shot.raw
        return raw[:SCREEN_BYTES], raw[SCREEN_BYTES:]

    def words(self, start: int, count: int) -> list[int]:
        read = self.lib.desmume_memory_read_long
        return [read(a) for a in range(start, start + 4 * count, 4)]

    def read(self, addr: int, length: int) -> bytes:
        """Bytes through the ARM9 bus, registers at their natural width."""
        lib, out, end = self.lib, bytearray(), addr + length
        while addr < end:
            left = end - addr
            if not addr & 3 and left >= 4:
                out += array.array("I", self.words(addr, left // 4)).tobytes()
                addr += left & ~3
            elif not addr & 1 and left >= 2:
                out += struct.pack("<H", lib.desmume_memory_read_short(addr))
                addr += 2
            else:
                out.append(lib.desmume_memory_read_byte(addr))
                addr += 1
        return bytes(out)

    def main_ram(self) -> tuple[bytes, int | None]:
        """The 4 MiB of main RAM and, when it hides one of the mirrors read, the DTCM's address."""
        count, block = MAIN_RAM_SIZE // 4, DTCM_SIZE // 4
        first = self.words(MAIN_RAM, count)
        second = self.words(MAIN_RAM + MAIN_RAM_SIZE, count)
        dtcm = None
        if first != second:
            for index in range(0, count, block):
                if first[index:index + block] == second[index:index + block]:
                    continue
                if dtcm is not None:
                    raise OracleError("main RAM differs from its mirror in more than one 16 KiB block")
                third = self.words(MAIN_RAM + 2 * MAIN_RAM_SIZE + 4 * index, block)
                if third == second[index:index + block]:
                    dtcm = MAIN_RAM + 4 * index
                    first[index:index + block] = third
                elif third == first[index:index + block]:
                    dtcm = MAIN_RAM + MAIN_RAM_SIZE + 4 * index
                else:
                    raise OracleError("three mirrors of main RAM disagree")
        return array.array("I", first).tobytes(), dtcm

    def registers(self, cpu: str) -> dict[str, int]:
        read = self.lib.desmume_memory_read_register
        values = {f"r{i}": read(f"{cpu}.r{i}".encode()) for i in range(16)}
        values["cpsr"] = read(f"{cpu}.cpsr".encode())
        values["spsr"] = read(f"{cpu}.spsr".encode())
        # r15 is the fetch address: two instructions past the one that runs next.
        values["pc"] = (values["r15"] - (4 if values["cpsr"] & 0x20 else 8)) & 0xFFFFFFFF
        return values


# ---------------------------------------------------------------- output

def write_ppm(path: Path, width: int, height: int, rgb: bytes) -> None:
    path.write_bytes(b"P6\n%d %d\n255\n" % (width, height) + rgb)


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    stride = width * 3
    rows = b"".join(b"\0" + rgb[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def framed(template: str, frame: int, several: bool) -> Path:
    """FILE for one --at frame: {frame} filled in, or -NNNNNN added when frames would collide."""
    if "{frame" in template:
        return Path(template.format(frame=frame))
    path = Path(template)
    return path.with_name(f"{path.stem}-{frame:06d}{path.suffix}") if several else path


def split_stdout(quiet: bool):
    """Keep stdout for results; send what the core printf()s to stderr (or nowhere)."""
    sys.stdout.flush()
    ours = os.dup(1)
    os.dup2(os.open(os.devnull, os.O_WRONLY) if quiet else 2, 1)
    return os.fdopen(ours, "w", encoding=sys.stdout.encoding or "utf-8", errors="replace")


# ---------------------------------------------------------------- the run

def run(args, out) -> int:
    rom = args.rom.resolve()
    if not rom.is_file():
        raise OracleError(f"{args.rom}: no such ROM")
    frames = args.frames
    if frames < 0:
        raise OracleError("--frames wants 0 or more")

    keys = [parse_keys(s) for s in args.keys]
    touches = [parse_touch(s) for s in args.touch]
    pulses = lid_pulses([parse_lid(s) for s in args.lid])
    mem = [parse_mem(s) for s in args.dump_mem]
    rtc = parse_rtc(args.rtc_time)
    at = sorted({parse_frame(s, "--at") for s in args.at})
    if any(f > frames for f in at):
        raise OracleError(f"--at {max(at)} is past --frames {frames}")
    if args.every is not None and args.every < 1:
        raise OracleError("--every wants 1 or more")
    if (args.dump_ram or mem or args.regs) and not at:
        raise OracleError("--dump-ram, --dump-mem and --regs need --at FRAME")
    wants_frames = bool(args.dump_frames or args.hash_frames or args.video_regs)
    if args.every and not wants_frames:
        raise OracleError("--every needs --dump-frames, --hash-frames or --video-regs")
    if args.no_ppm and not args.png:
        raise OracleError("--no-ppm needs --png")
    captures = set(at)
    if args.every:
        captures |= set(range(args.every, frames + 1, args.every))

    args.save_dir.mkdir(parents=True, exist_ok=True)
    save_dir = args.save_dir.resolve()
    if len(str(save_dir)) + len(rom.stem) + 12 > 259:
        raise OracleError(f"{save_dir}: too long for DeSmuME's 260-character save path with this ROM name")
    if args.dump_frames:
        args.dump_frames.mkdir(parents=True, exist_ok=True)
    listing = None
    if args.dump_frames and args.hash_frames:
        listing = (args.dump_frames / "frames.txt").open("w", encoding="ascii", newline="\n")

    def emit(line: str, listed: bool = False) -> None:
        print(line, file=out, flush=True)
        if listed and listing:
            listing.write(line + "\n")

    core = None
    status = 0
    try:
        core = Core(save_dir)
        lib = core.lib
        core.emu.open(str(rom))
        if not args.host_rtc:
            from desmume.emulator import DeSmuME_Date, StartFrom
            date = DeSmuME_Date(rtc.year, rtc.month, rtc.day, rtc.hour, rtc.minute, rtc.second, 0)
            core.emu.movie.record(str(save_dir / "oracle.dsm"), "oracle_desmume", StartFrom.START_BLANK, "", date)
            if not core.emu.movie.is_recording():
                raise OracleError("DeSmuME did not start the movie that pins its clock")
            lib.desmume_resume()

        several = len(at) > 1
        first_picture: dict[str, int | None] = {"top": None, "bottom": None}

        def capture(frame: int) -> None:
            top, bottom = core.screens()
            if args.dump_frames:
                pictures = ([("", SCREEN_H * 2, top + bottom)] if args.stacked else
                            [("-top", SCREEN_H, top), ("-bottom", SCREEN_H, bottom)])
                for suffix, height, rgb in pictures:
                    stem = args.dump_frames / f"{frame:06d}{suffix}"
                    if not args.no_ppm:
                        write_ppm(stem.with_suffix(".ppm"), SCREEN_W, height, rgb)
                    if args.png:
                        write_png(stem.with_suffix(".png"), SCREEN_W, height, rgb)
            if args.hash_frames:
                emit(f"frame {frame} top={hashlib.sha256(top.translate(TO_5_BITS)).hexdigest()} "
                     f"bottom={hashlib.sha256(bottom.translate(TO_5_BITS)).hexdigest()}", True)
            if args.video_regs:
                vram = [core.read(0x04000240 + i, 1)[0] for i in range(10)]
                counts = struct.unpack("<HH", core.read(0x04000604, 4))
                emit(f"video {frame} dispcnt_a={lib.desmume_memory_read_long(0x04000000):08X} "
                     f"dispcnt_b={lib.desmume_memory_read_long(0x04001000):08X} "
                     f"disp3dcnt={lib.desmume_memory_read_short(0x04000060):04X} "
                     f"powcnt1={lib.desmume_memory_read_short(0x04000304):04X} "
                     f"vramcnt={','.join(f'{v:02X}' for v in vram[:7] + vram[8:])} wramcnt={vram[7]:02X} "
                     f"gxstat={lib.desmume_memory_read_long(0x04000600):08X} "
                     f"polygons={counts[0]} vertices={counts[1]}", True)
            if frame not in at:
                return
            if args.regs:
                for cpu in ("arm9", "arm7"):
                    values = core.registers(cpu)
                    emit(f"regs {frame} {cpu} " + " ".join(f"{name}={value:08X}" for name, value in values.items()))
            if args.dump_ram:
                data, dtcm = core.main_ram()
                path = framed(args.dump_ram, frame, several)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
                if dtcm is not None:
                    emit(f"dtcm {frame} base=0x{dtcm:08X}")
            for addr, length, name in mem:
                data = core.read(addr, length)
                if name:
                    path = framed(name, frame, several)
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                else:
                    emit(f"mem {frame} 0x{addr:08X} {length}: {data.hex(' ')}")

        started = time.perf_counter()
        if 0 in captures:
            capture(0)
        touching = False
        for frame in range(frames):
            pad = 0
            for start, mask, duration in keys:
                if start <= frame < start + duration:
                    pad |= mask
            if frame in pulses:
                pad |= 1 << LID_BIT
            lib.desmume_input_keypad_update(pad)
            point = None
            for start, x, y, duration in touches:
                if start <= frame < start + duration:
                    point = (x, y)
            if point:
                lib.desmume_input_set_touch_pos(*point)
                touching = True
            elif touching:
                lib.desmume_input_release_touch()
                touching = False
            try:
                lib.desmume_cycle(0)
            except OSError as error:  # a hardware exception inside the core, as ctypes reports it
                raise OracleError(f"DeSmuME crashed while emulating frame {frame} ({error}); "
                                  f"captures up to {frame} are complete") from None
            if not lib.desmume_running():
                raise OracleError(f"DeSmuME stopped emulating in frame {frame}")
            done = frame + 1
            if args.first_nonblank and None in first_picture.values():
                for name, rgb in zip(("top", "bottom"), core.screens()):
                    if first_picture[name] is None and rgb != rgb[:3] * (SCREEN_BYTES // 3):
                        first_picture[name] = done
            if done in captures:
                capture(done)
        elapsed = time.perf_counter() - started

        if args.first_nonblank:
            emit("first_nonblank " + " ".join(f"{name}={'none' if value is None else value}"
                                              for name, value in first_picture.items()))
        if not args.host_rtc:
            core.emu.movie.stop()
        if core.violations:
            raise OracleError("DeSmuME touched files outside the save directory: " + "; ".join(core.violations))
        if args.verbose:
            for path, mode in core.opened:
                print(f"opened {path} ({mode})", file=sys.stderr)
        if not args.quiet:
            rate = f"{frames / elapsed:.0f} frames/s" if elapsed > 0 and frames else "-"
            print(f"rom={rom.name} frames={frames} captures={len(captures)} {elapsed:.1f} s ({rate}) "
                  f"rtc={'host' if args.host_rtc else rtc} "
                  f"save-dir={'(temporary)' if args.temporary_save_dir else save_dir}",
                  file=sys.stderr)
    except OracleError as error:
        print(f"oracle_desmume: {error}", file=sys.stderr)
        status = 1
    finally:
        if listing:
            listing.close()
        try:
            if core:
                core.emu.destroy()
        finally:
            ctypes.CDLL("ucrtbase").fflush(None)
    return status


class ArgumentParser(argparse.ArgumentParser):
    """@FILE arguments: any number of options per line, # starts a comment."""

    def convert_arg_line_to_args(self, arg_line: str) -> list[str]:
        return arg_line.split("#", 1)[0].split()


def main() -> int:
    parser = ArgumentParser(description=__doc__.splitlines()[0], fromfile_prefix_chars="@",
                            epilog="See the comment at the top of this file for frame numbering, "
                                   "output formats and what is isolated. @FILE reads more options from "
                                   "FILE (several per line, # comments), which is how input scripts are kept.")
    parser.add_argument("rom", type=Path, help="the .nds file (opened read-only, never copied)")
    parser.add_argument("--frames", type=int, required=True, help="frames to emulate")
    parser.add_argument("--keys", action="append", default=[], metavar="FRAME:BUTTONS[:N]",
                        help=f"hold BUTTONS (A+B+START...) for N frames (default {DEFAULT_FRAMES})")
    parser.add_argument("--touch", action="append", default=[], metavar="FRAME:X,Y[:N]",
                        help=f"hold the stylus at bottom-screen pixel X,Y for N frames (default {DEFAULT_FRAMES})")
    parser.add_argument("--lid", action="append", default=[], metavar="FRAME:N",
                        help=f"close the lid for N frames (at least {LID_MIN_FRAMES} in DeSmuME)")
    parser.add_argument("--dump-frames", type=Path, metavar="DIR", help="write both screens at every capture")
    parser.add_argument("--every", type=int, metavar="K", help="capture after every K frames")
    parser.add_argument("--at", action="append", default=[], metavar="FRAME",
                        help="capture after FRAME frames (0 or 'boot': before the first)")
    parser.add_argument("--stacked", action="store_true", help="one 256x384 image instead of top and bottom")
    parser.add_argument("--png", action="store_true", help="also write PNG")
    parser.add_argument("--no-ppm", action="store_true", help="with --png: PNG only")
    parser.add_argument("--hash-frames", action="store_true", help="print a SHA-256 of each screen at every capture")
    parser.add_argument("--video-regs", action="store_true",
                        help="print DISPCNT, DISP3DCNT, POWCNT1, VRAMCNT and the 3D list sizes at every capture")
    parser.add_argument("--first-nonblank", action="store_true",
                        help="report the first frame after which each screen is not one flat colour")
    parser.add_argument("--dump-ram", metavar="FILE", help="main RAM (4 MiB) at each --at frame")
    parser.add_argument("--dump-mem", action="append", default=[], metavar="ADDR:LEN[:FILE]",
                        help="memory as the ARM9 sees it at each --at frame (hex on stdout without FILE)")
    parser.add_argument("--regs", action="store_true", help="ARM9 and ARM7 registers at each --at frame")
    parser.add_argument("--save-dir", type=Path, metavar="DIR",
                        help="where DeSmuME keeps the battery save (default: a temporary directory)")
    parser.add_argument("--rtc-time", default=DEFAULT_RTC, metavar='"YYYY-MM-DD HH:MM:SS"',
                        help=f"real-time clock at frame 0 (default {DEFAULT_RTC})")
    parser.add_argument("--host-rtc", action="store_true", help="follow the host clock instead (not repeatable)")
    parser.add_argument("--quiet", action="store_true", help="drop the emulator's log and the summary line")
    parser.add_argument("--verbose", action="store_true", help="list every file the emulator opened")
    parser.add_argument("--temporary-save-dir", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.save_dir is None:
        # The core keeps the save file open until the process ends, so a
        # temporary save directory can only be removed from outside it.
        with tempfile.TemporaryDirectory(prefix="nds-oracle-", ignore_cleanup_errors=True) as scratch:
            return subprocess.call([sys.executable, str(Path(__file__).resolve()), *sys.argv[1:],
                                    "--save-dir", scratch, "--temporary-save-dir"])

    out = split_stdout(args.quiet)
    try:
        return run(args, out)
    except OracleError as error:
        print(f"oracle_desmume: {error}", file=sys.stderr)
        return 1
    finally:
        out.flush()


if __name__ == "__main__":
    sys.exit(main())
