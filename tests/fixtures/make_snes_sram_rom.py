#!/usr/bin/env python3
"""Create a small, repository-owned SNES ROM with battery-backed SRAM.

A 32 KiB LoROM image (cartridge type $02: ROM, RAM, battery; 2 KiB SRAM)
whose program writes $A5 and $5A to SRAM at $70:0000, then idles with NMI
off. Used where a test needs a game that writes a battery save: the macOS CI
job checks that an app bundle writes it to Application Support.

    python tests/fixtures/make_snes_sram_rom.py --output build-ci/snes_sram.sfc
"""

from __future__ import annotations

import argparse
from pathlib import Path

PROGRAM = bytes(
    [
        0x78,                    # 8000 SEI
        0x18,                    # 8001 CLC
        0xFB,                    # 8002 XCE        native mode
        0xE2, 0x30,              # 8003 SEP #$30   8-bit A, X, Y
        0x9C, 0x00, 0x42,        # 8005 STZ $4200  no NMI, IRQ or auto-joypad
        0xA9, 0xA5,              # 8008 LDA #$A5
        0x8F, 0x00, 0x00, 0x70,  # 800A STA $700000
        0xA9, 0x5A,              # 800E LDA #$5A
        0x8F, 0x01, 0x00, 0x70,  # 8010 STA $700001
        0x80, 0xFE,              # 8014 BRA $8014
    ]
)


def make_rom() -> bytearray:
    rom = bytearray([0xFF]) * 0x8000
    rom[0 : len(PROGRAM)] = PROGRAM
    header = 0x7FC0
    rom[header : header + 21] = b"NATIVE CONSOLE SRAM  "
    rom[header + 0x15] = 0x20  # LoROM, SlowROM
    rom[header + 0x16] = 0x02  # ROM + RAM + battery
    rom[header + 0x17] = 0x05  # 32 KiB ROM
    rom[header + 0x18] = 0x01  # 2 KiB SRAM
    rom[header + 0x19] = 0x01  # North America
    rom[header + 0x1A] = 0x00
    rom[header + 0x1B] = 0x00
    # Every vector points at the program (only reset is used).
    for vector in range(0x7FE0, 0x8000, 2):
        rom[vector] = 0x00
        rom[vector + 1] = 0x80
    rom[header + 0x1C : header + 0x20] = b"\xFF\xFF\x00\x00"
    total = sum(rom) & 0xFFFF
    rom[header + 0x1E] = total & 0xFF
    rom[header + 0x1F] = total >> 8
    complement = total ^ 0xFFFF
    rom[header + 0x1C] = complement & 0xFF
    rom[header + 0x1D] = complement >> 8
    return rom


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(make_rom())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
