# Super Nintendo backend: plan

Started 2026-09-11 on branch `feature/snes-backend`, following
[ADDING-A-CONSOLE.md](ADDING-A-CONSOLE.md). This is the working plan for
turning `.sfc`/`.smc` ROMs into native executables the way the Game Boy and
GBA paths do: recompile the ROM to C, configure with CMake, build with Ninja,
launch from the hub. It says what we reuse, how the phases are gated, and how
every item of the SNES assembly-to-C nuances checklist is tested
([Checklist coverage](#checklist-coverage)).

## Ground rules

1. **License.** Native Console is MIT. Code is copied only from MIT, BSD,
   Apache-2.0, zlib, ISC or CC0 sources and recorded in `THIRD_PARTY.md`.
   ares (ISC) is the main behaviour reference and the source of the ported
   PPU, S-DSP and SMP code. bsnes and Mesen2 (GPL-3) and snes9x
   (non-commercial) are not read for code. Test data without a license is
   used locally only, never committed.
2. **No copyrighted firmware.** The SNES has no CPU BIOS. The SPC700's 64-byte
   boot ROM is our own program implementing the documented upload protocol
   (`runtime-snes/src/snes_apu.c`).
3. **One semantics source.** The interpreter and generated C call the same
   force-inlined handlers `snes_op_XX(c, mode)` in
   `runtime-snes/include/snes_cpu_impl.h`. The mode argument (E, M, X) is a
   constant in generated code, so the width branches fold away. The vectors
   validate the handlers once; the emitter only sequences them.
4. **Guest state is explicit.** Registers and flags live in `SnesCpu`; every
   memory access goes through the bus (`snes_bus.c`). No guest value lives in
   a C local across a bus access.
5. **Mode is part of the code address.** The same bytes decode differently in
   8- and 16-bit modes, so the recompiler's unit is a key: 24-bit PC plus
   mode. Code entered in a mode it was never compiled for runs interpreted.
6. **Interpreter from day one.** WRAM code, missed indirect targets and code
   after PLP/RTI fall back to it. It is also the differential oracle.
7. **Evidence layers stay labelled** (`AGENTS.md`): unit tests, differential
   against our own interpreter, and independent evidence (hardware-verified
   test ROMs, vectors). Only the third proves hardware accuracy.
8. **Fail closed.** Missing test data, a timeout, an empty selection or a ROM
   that does not finish is a failure.

## What we reuse

| Source | License | What we take |
|---|---|---|
| [SingleStepTests/65816](https://github.com/SingleStepTests/65816) | none | Per-instruction vectors with bus cycles, 10,000 native + 10,000 emulation per opcode (5.12 M). Local test data only. |
| [SingleStepTests/spc700](https://github.com/SingleStepTests/spc700) | MIT | Per-instruction SPC700 vectors with bus cycles (256,000). |
| [gilyon/snes-tests](https://github.com/gilyon/snes-tests) v1.4 | MIT | `cputest-full/basic` (every 65C816 opcode and addressing mode, wrapping, decimal; verified on hardware) and `spctest`. |
| [undisbeliever/snes-test-roms](https://github.com/undisbeliever/snes-test-roms) | zlib | HDMA, INIDISP, DMA behaviour ROMs (visual). |
| [absindx/SNES-TestRoms](https://github.com/absindx/SNES-TestRoms) | BSL-1.0 | SA-1 RAM protection and reboot tests. |
| [PeterLemon/SNES](https://github.com/PeterLemon/SNES) | none | krom's CPU/SPC700 instruction tests and PPU demos with reference screenshots. Local only. |
| [higan snes-test-roms](https://gitlab.com/higan/snes-test-roms) (mirror) | mixed, none | blargg's SPC tests, timing/IRQ/HDMA ROMs. Local only. |
| [ares](https://github.com/ares-emulator/ares) af4cbb0 | ISC | Behaviour reference for the 65816 bus sequences and S-CPU timing; ported: the scanline PPU (`ppu-performance`), the S-DSP, the SMP memory map and timers, the SPC700 bus sequences, DMA/HDMA channel logic, the multiplier/divider steps, the SA-1 (memory maps, registers, DMA and character conversion, arithmetic, bit reader, timer, interrupts). |
| fullsnes (problemkaputt.de) | reference | Hardware reference. |

`tools/fetch_snes_tests.py` downloads every suite at a pinned commit (or a
release pinned by SHA-256) into `roms/snes/` and writes `MANIFEST.json`.

## Layout

```text
runtime-snes/
  include/snes_cpu.h        65C816 state (shared by the S-CPU and the SA-1)
  include/snes_cpu_impl.h   instruction handlers, force-inlined; opcode table
  include/snes_cpu_decode.h opcode classification (analyzer, emitter, tools)
  include/snes_bus.h        bus interface the CPU calls
  include/snes.h            machine: WRAM, cartridge map, I/O, DMA, timing
  include/snes_recomp.h     interface for recompiled code
  src/snes_cpu.c            interpreter, reset, interrupt entry
  src/snes.c                scheduler, video timing, interrupts, run loop
  src/snes_bus.c            memory map, access speeds, open bus, B-bus
  src/snes_cart.c           header detection, LoROM/HiROM/ExHiROM page map
  src/snes_sa1.c            SA-1: both CPUs' maps, registers, DMA, run loop
  src/snes_io.c             $4200-$421F, WRAM port, joypads, multiplier/divider
  src/snes_dma.c            DMA and HDMA
  src/snes_ppu*.c           PPU registers and scanline renderer (from ares)
  src/snes_apu.c, snes_spc700.c, snes_dsp.c   SMP, SPC700, S-DSP (from ares)
  src/snes_recomp.c         program install (SHA-256), hashed lookup, WRAM checks
  src/snes_coverage.c       interpreted-code report and traces for --trace
  src/snes_state.c          save states (the whole machine behind a ROM hash)
  src/snes_runner.c         headless runner and --differential
  platform/                 SDL2 frontend and overlay menu
  tools/snes_run.c, snes_play.c
recompiler-snes/src/        rom, analyzer, emitter, CLI (snesrecomp)
tests/snes/                 SST harnesses, PPU, APU and SA-1 unit tests, ROM baseline
hub/src/console_snes.cpp    hub backend
tools/fetch_snes_tests.py
tools/snes/                 run_snes_tests.py, recomp_suite.py, asm65816.py,
                            disasm65816.py, compare_frame.py, checklist/
```

## Phases

### P0: research, licenses, test data

Gate: the fetch script runs clean twice and the second run does nothing.

### P1: 65C816 and SPC700 cores

- 65C816: all 256 opcodes in both emulation and native mode; bus cycles
  ordered as the hardware performs them (ares's sequences): dummy writes of
  read-modify-write instructions in emulation mode, the extra cycle for a
  non-zero D low byte, index page-cross cycles, emulation-mode branch page
  cycles, 16-bit stack moves of the 65816-only instructions in emulation mode.
- SPC700: all 256 opcodes with their bus cycles.
- Gate: 100 % of both vector sets in Release, Debug, AddressSanitizer and
  UBSan builds; deviations listed below.

### P2: machine and headless runner

- LoROM, HiROM and ExHiROM maps as 4 KiB page tables with the cartridge
  mirroring of non-power-of-two ROMs; WRAM and its mirrors; open bus (MDR);
  access speeds (6/8/12 clocks, FastROM); DMA and HDMA; NMI, H/V IRQ,
  auto-joypad, the multiplier/divider with per-cycle results; APU clocked
  against the master clock exactly.
- Runner: `snes_run ROM --frames N`, `--until-idle`, `--expect-text`,
  `--fail-text`, `--apu-console`, `--dump-frame`, `--dump-state`,
  `--keys`, `--auto-input`, `--trace-last`, `--trace-apu`.
- Gate: gilyon cputest and spctest pass; `tests/snes/rom_baseline.txt`.

### P3: video

- ares's scanline PPU: modes 0-7, hires, interlace, mosaic, windows, color
  math, offset-per-tile, sprites with the high table and range/time over.
- Gate: PPU unit tests; krom PPU demos compared with their reference
  screenshots (`tools/snes/compare_frame.py`). The screenshots are captures,
  some quantized or joypad-scrolled, so mismatches are checked by eye.

### P4: recompiler

- `snesrecomp`: the analyzer explores (PC, mode) keys from the vectors
  (native vectors in all four M/X combinations) and trace entries, tracks
  REP/SEP, knows the mode after XCE when CLC or SEC precedes it, reads jump
  tables for JMP/JSR (a,X), and leaves for the dispatcher after PLP, RTI,
  calls, returns and indirect jumps. The emitter writes one C block per
  instruction: the opcode fetch, `snes_op_XX(c, mode)`, then a goto, a
  fall-through or a return. WRAM code is compiled from the trace's WRAM
  image and checked against live WRAM before it runs; a native WRAM routine
  leaves as soon as a code page is written.
- Guessed code is validated. Code known to run (trace keys, the reset
  vector) is accepted as is; everything reached by a guess (a callee in its
  caller's mode, a call's return site, a jump-table entry, a PLP or COP
  continuation, the other vectors) is explored region by region and kept
  only if no instruction is BRK, COP, WDM or STP, none runs off code or
  straddles accepted code, and the region is not implausibly large. A
  rejected region takes the guesses it would have spawned with it. Without
  this, a 4 MiB ROM snowballed into 15.5 million "instructions" (every byte
  in every mode) as data decoded as code called into more data. With a trace,
  guessing stops 4 levels from known code (`--speculation`).
- Every compiled instruction is a resume point: code yields after any
  instruction (an event, or the SA-1 reaching the S-CPU's time), and the
  dispatcher continues there natively.
- Generated code performs the interpreter's bus cycles, yields when an event
  is due or an interrupt is pending, and I/O accesses catch up on due events.
- `--differential`: CPU registers and cycle counter after every native slice;
  WRAM, VRAM, CGRAM, OAM, audio RAM and battery RAM every frame.
- Gate: every test ROM gives the same verdict recompiled as interpreted, and
  runs clean in `--differential` (`tools/snes/recomp_suite.py`).

### P5: audio, saves, frontend

- S-DSP and SPC700 (done in P1/P2), battery RAM (`.sav`), SDL2 frontend with
  the overlay menu (keyboard and controller, volume, speed, scaling, save
  states), `snes_play`.
- Gate: a 30-second windowed run without audio underruns; the overlay menu
  opens from a controller alone, pauses, and its settings survive a restart.

### P6: hub integration and the first game

- The user's game, Kirby's Dream Land 3 (USA), is an SA-1 cartridge (map mode
  $23, type $35): the SA-1 (a second 65C816 at 10.74 MHz with its own DMA,
  character conversion, arithmetic, timers, BW-RAM and I-RAM) is part of this
  phase. The SA-1 reuses the 65C816 core with its own bus (`SnesCpu.id`), and
  recompiled functions serve both CPUs, since both execute from the same
  cartridge ROM map.
- Scheduling: the SA-1 lags the S-CPU and is caught up before every S-CPU
  access to shared state (SA-1 registers, I-RAM, BW-RAM, the vector
  override) and whenever S-CPU events run. The SA-1 cannot see WRAM or the
  PPU, so nothing else is shared. Catch-ups happen at the same S-CPU bus
  cycles whether the S-CPU is native or interpreted, and the SA-1 stops at
  the first instruction boundary past the target in both, so the two
  timelines, and `--differential`, stay exact.
- Recompiled code assumes the power-on Super MMC banking (banks C-F = the
  first 4 MiB in order); if a game switches banks, native code is disabled
  until the banking is linear again.
- Gate: the game imports, builds, plays and gets a cover in the hub; its save
  survives a rebuild; `--differential` is clean over 6,000 scripted frames;
  `--coverage` reports the native share.

### P7: platforms and export; P8: accuracy and speed

As in ADDING-A-CONSOLE.md. P8 items already known: dot-accurate H/V IRQ
positions (irregular dots 323/327), the "idle cycle becomes a read" of
implied instructions when an interrupt follows, the irqLock delay after DMA
and NMITIMEN writes, mid-scanline PPU register effects, constant operand
fetches in generated code.

## Checklist coverage

Numbers are the items of the SNES assembly-to-C nuances checklist. Evidence:

- **SST** — SingleStepTests 65816, 5,120,000 vectors (registers, flags, memory
  and the ordered bus cycles), through the interpreter and so through the
  handlers generated code calls.
- **SST-SPC** — SingleStepTests SPC700, 256,000 vectors.
- **gilyon** — gilyon's cputest-full (1,609 tests, verified on hardware) and
  spctest (1,367 tests), both passing on the full machine.
- **krom** — krom's CPU (23) and SPC700 (7) test ROMs; **blargg** — blargg's
  SPC DSP, timer and memory-timing ROMs.
- **CL n** — test *n* of the project's checklist ROM
  (`tools/snes/checklist/make_checklist_rom.py`, list below). The ROM runs in
  the interpreter, recompiled, and in `--differential` lockstep, so every CL
  test checks both the interpreter and the generated C.
- **PPU:** / **APU:** — named unit tests in `tests/snes/test_snes_ppu.c` and
  `test_snes_apu.c`.
- **construction** — holds by design (ground rules 3-5); cited code path.

| # | Nuance | Evidence |
|---|---|---|
| 1 | M/X flags set the widths | SST (every opcode in all widths); gilyon; CL 0 |
| 2 | 8/16-bit register high bytes | SST; CL 1 (B survives 8-bit loads) |
| 3 | Direct page follows D | SST (random D); gilyon; CL 2 (D=$0300, D=$0301) |
| 4 | Data bank register | SST; CL 3 |
| 5 | Program bank register | SST (JSL, JML, RTL, PHK); CL 4 |
| 6 | 24-bit addresses | SST; CL 5 |
| 7 | 8/16-bit wraparound | SST; CL 6 |
| 8 | Carry in ADC/SBC | SST; krom ADC/SBC; CL 7 |
| 9 | Decimal mode ADC/SBC | SST (random D flag); gilyon decimal tests; krom BCD pages; CL 8 |
| 10 | V differs from C | SST; CL 9 |
| 11 | N by width | SST; CL 10 |
| 12 | Z by width | SST; CL 10 |
| 13 | Compares set flags only | SST; CL 11 |
| 14 | BIT behaviour | SST (BIT #, dp, abs, indexed); CL 12 |
| 15 | ASL/LSR/ROL/ROR and carry | SST; CL 13 |
| 16 | XCE | SST (FB in both modes); CL 14 |
| 17 | Emulation mode | SST emulation-mode vectors (2.56 M); gilyon wrap tests; CL 14, CL 42 |
| 18 | Stack is CPU state | SST; CL 15 |
| 19 | PHA/PLA size follows M | SST; CL 15 |
| 20 | PHX/PLX size follows X | SST; CL 15 |
| 21 | Interrupts push state | SST (BRK, COP, RTI in both modes); CL 16; NMI/IRQ entry in CL 17, CL 39 |
| 22 | NMI in V-blank | CL 17 |
| 23 | DMA is hardware | CL 18 (VRAM round trip, fixed source, direction, no WRAM-to-WRAM) |
| 24 | HDMA is per line | CL 19; krom HDMA demos match their screenshots |
| 25 | PPU registers have side effects | CL 20; PPU: vram_increment_and_remap, bgofs_double_write_latch |
| 26 | VRAM addressing | CL 20; PPU: vram_increment_and_remap, vram_read_prefetch, vram_write_blocked_active_display |
| 27 | CGRAM color format | CL 21; PPU: cgram_latch_format_xrgb |
| 28 | OAM high table | CL 22; PPU: oam_low_high_table_sprite |
| 29 | Tile and VRAM addressing | PPU: render_mode1_bg_tile, render_16x16_tile, render_mode7_identity; krom BGMAP/HiColor demos |
| 30 | Bits packed in registers | CL 23; PPU: write_only_reads_open_bus |
| 31 | Read-modify-write | CL 23 (TSB/TRB; the emulation-mode dummy write lands on $2180 twice); SST (the dummy write cycle) |
| 32 | Old register state as input | CL 24; construction (registers are machine state, never reinitialised per routine) |
| 33 | Flags as implicit inputs | CL 7; SST |
| 34 | Flags as outputs | CL 25, CL 24 |
| 35 | Branches depend on flags | SST (all branches); krom BRA; CL 25 |
| 36 | Branch range and bank wrap | SST; CL 26 (a branch at $01:FFFE lands in $01:0040) |
| 37 | JSL/JML vs JSR/JMP | SST; CL 4, CL 27 |
| 38 | RTL vs RTS | SST; CL 4 (three bytes vs two) |
| 39 | Indirect addressing details | SST; gilyon; CL 27, CL 42 |
| 40 | 16-bit pointers crossing banks | SST; CL 5 |
| 41 | Self-modifying code | CL 28; construction: WRAM code is checked against live WRAM before it runs, and native WRAM code leaves when a code page is written (`snes_recomp.c`, emitter) |
| 42 | Code copied to WRAM | CL 28, CL 26, CL 29 |
| 43 | WRAM vs ROM timing | CL 29 (FastROM loop runs more iterations per frame; WRAM equals SlowROM) |
| 44 | Timing is gameplay | CL 29, CL 30; `--differential` is cycle-exact |
| 45 | Multiplier and divider | CL 30 (products, quotients, divide by zero) |
| 46 | Sign extension | CL 31 |
| 47 | Index register wrap | SST; CL 6 |
| 48 | TAX/TXA width | SST; CL 1 |
| 49 | XBA sets N and Z | SST (EB); CL 33 |
| 50 | REP/SEP kept | SST; CL 0; construction: the analyzer tracks the mode through REP/SEP (`analyzer.cpp`) |
| 51 | PLP/RTI change the mode | SST; CL 16; construction: generated code returns to the dispatcher after PLP and RTI |
| 52 | Write-only registers | CL 34; PPU: write_only_reads_open_bus |
| 53 | Mirrored memory | CL 35, CL 26 |
| 54 | Open bus | CL 34 ($2100, $5000 and a long read return the last bus byte) |
| 55 | ROM banking and mappers | CL 35; `snes_cart.c` page tables; SA-1 Super MMC banking: `test_snes_sa1` (`super_mmc`, `vector_override`) |
| 56 | Game loop vs interrupts | CL 17 |
| 57 | SPC700 | SST-SPC; gilyon spctest; krom SPC700; blargg DSP, timer, memory timing |
| 58 | APU handshaking | CL 36 (boot protocol, upload, command round trip); APU: apu_handshaking, boot_upload_and_run |
| 59 | Controller input registers | CL 37 (auto-read with scripted input; serial read agrees) |
| 60 | PRNG | CL 32 (16-bit LFSR, expected value computed in Python) |
| 61 | Fixed point | CL 32 |
| 62 | Rounding and truncation | CL 31 |
| 63 | Table lookups | CL 32 |
| 64 | LUT index wrap | CL 32 (8-bit X wraps $FF to $00) |
| 65 | Multi-byte pointers | CL 27 ([dp] vs (dp)+DBR) |
| 66 | Endianness | CL 33 |
| 67 | Struct packing | CL 33; construction (guest memory is a byte array) |
| 68 | No alignment | CL 33 (words at odd addresses) |
| 69 | Indirect data tables | CL 27 |
| 70 | Direct-page pointer wrap | CL 27 (emulation-mode (dp) wraps within the page); SST; gilyon |
| 71 | Decimal vs hex literals | CL 40; construction: the recompiler reads bytes, not source |
| 72 | Memory-mapped game state | CL 33; construction (WRAM at its real addresses) |
| 73 | Aliasing between routines | CL 33 |
| 74 | Byte order and XBA | CL 33 |
| 75 | Signed comparisons | CL 11 (N xor V) |
| 76 | Stack-based parameters | CL 41; construction: RTS returns to the dispatcher, so a moved return address is followed |
| 77 | Return values in A/X/Y | CL 24 |
| 78 | Register preservation | CL 24 |
| 79 | Caller/callee-saved | CL 24; construction |
| 80 | Carry as a boolean | CL 24 |
| 81 | Bit streams with ROL/ROR | CL 13 |
| 82 | Multiplier result timing | CL 30 (a read three cycles after the write sees the partial product $68) |
| 83 | Wait loops | CL 19, CL 37 (polling $4212 terminates natively at /O2) |
| 84 | Frame counters | CL 17 |
| 85 | Input repeat/debounce | CL 37 (a press counts once) |
| 86 | DMA order | CL 18 (channel 0 before channel 1) |
| 87 | Register write order | CL 38 (M7A byte order); PPU: bgofs_double_write_latch, m7_multiplier |
| 88 | Scanline order | CL 39 (V-timer IRQ on line 100); CL 19 |
| 89 | Music/SFX priority | The game's own sound driver runs on the emulated SPC700 (57, 58); nothing is translated |
| 90 | ROM data as constant memory | CL 35; krom tests read ROM tables |
| 91 | Compiler optimisation | Construction: all guest memory goes through the bus; generated code built at /O2 passes the CL ROM and differential; UBSan/ASan builds |
| 92 | Volatile / memory-mapped I/O | Construction (`snes_bus.c` is the abstraction layer); CL 19, CL 37 |
| 93 | Original vs gameplay timing | `--differential` (cycle-exact); CL 29 |
| 94 | Not line by line blindly | Method: per-instruction recompilation from one semantics source, validated by the layers above |
| 95 | CPU state structure | `SnesCpu` holds exactly these fields, plus the data bus latch and WAI/STP |
| 96 | Memory model | Page-table bus plus named devices (PPU, APU, DMA, I/O) |
| 97 | Two conversion goals | This project is goal A (faithful) |
| 98 | Most common dangers | Each maps to rows above (1, 8, 3, 4-5, 7, 61, 60, 25, 23-24, 22, 57, 67, 46, 76-77, 75) |
| 99 | Translation strategy | The analyzer determines the mode per instruction; inputs and outputs are machine state |
| 100 | Golden rule | The evidence layers above |

### Checklist ROM tests

| CL | Items | What it checks |
|---|---|---|
| 0 | 1, 50 | REP/SEP switch the accumulator width; an 8-bit load keeps the high byte |
| 1 | 2, 48 | 8-bit A keeps B; TAX copies 16 bits when X is 16-bit, 8 when 8-bit |
| 2 | 3 | Direct page follows D |
| 3 | 4 | Absolute data addresses use DBR |
| 4 | 5, 37, 38 | JSL sets PBR, PHK sees it, RTL pops three bytes, JSR/RTS two |
| 5 | 6, 40, 90 | 24-bit addresses; long,X and abs,X carry into the next bank |
| 6 | 7, 47 | Wraparound of A and the index registers |
| 7 | 8, 33 | Carry is an input to ADC/SBC; a 32-bit add chains it |
| 8 | 9 | Decimal mode, 8 and 16 bits, carry out |
| 9 | 10 | V and C differ |
| 10 | 11, 12 | N and Z follow the width |
| 11 | 13, 75 | Compares; unsigned and signed outcomes |
| 12 | 14 | BIT copies N and V from memory; BIT # changes Z only |
| 13 | 15, 81 | Rotates use carry; bit streams |
| 14 | 16, 17 | XCE; M/X forced; the stack wraps in page 1 |
| 15 | 18-20 | PHA/PLA, PHX/PLX sizes |
| 16 | 21, 51 | COP pushes state; the handler changes widths; RTI restores them |
| 17 | 22, 56, 84 | NMI in V-blank once per frame |
| 18 | 23, 86 | DMA |
| 19 | 24 | HDMA writes one table entry per line |
| 20 | 25, 26 | VRAM port increments; the read latch lags a word |
| 21 | 27 | CGRAM pairs, 15 bits |
| 22 | 28 | OAM low and high tables |
| 23 | 30, 31 | TSB/TRB; emulation-mode RMW writes a register twice |
| 24 | 32, 77-80 | Inputs in A, outputs in carry and Y, X preserved |
| 25 | 34, 35 | INC/DEX leave C for a later branch |
| 26 | 36, 42, 53 | Branch at the end of a bank wraps into a WRAM mirror |
| 27 | 39, 65, 69, 70 | Indirect modes; emulation-mode (dp) page wrap |
| 28 | 41, 42 | WRAM code runs, and again after its bytes change |
| 29 | 43, 44 | FastROM vs SlowROM vs WRAM speed |
| 30 | 45, 82 | Multiplier, divider, divide by zero, partial product |
| 31 | 46, 62 | No implicit sign extension; shift rounding |
| 32 | 60, 61, 63, 64 | LFSR, 8.8 fixed point, 8-bit index wrap |
| 33 | 66-68, 72-74 | Endianness, odd addresses, aliasing, XBA |
| 34 | 52, 54 | Open bus |
| 35 | 53, 55 | WRAM and ROM mirrors, LoROM layout |
| 36 | 57, 58 | APU boot protocol and command round trip |
| 37 | 59, 85 | Auto-joypad and serial reads with scripted input |
| 38 | 87 | M7A byte order decides the PPU multiplier result |
| 39 | 88 | V-timer IRQ on the programmed line |
| 40 | 71 | Immediate operands as encoded |
| 41 | 76 | Parameters inline after JSR |
| 42 | 39 | (dp,X) emulation-mode page wrap with DL != 0 |

## Status

| Phase | State |
|---|---|
| P0 | Done 2026-09-11. Seven suites fetched (2.7 GB of 65816 vectors); the second run is a no-op. |
| P1 | Done 2026-09-11. 65C816: 5,120,000 / 5,120,000 vectors in MSVC Release, Debug and AddressSanitizer builds and a clang 11 ASan+UBSan build (Linux, Steam Runtime SDK), with 308 hardware deviations checked against the hardware rule (below). SPC700: 256,000 / 256,000 in MSVC Release, Debug and ASan, and gcc 13 ASan+UBSan. The PPU, APU and SA-1 unit tests are clean under clang ASan+UBSan, and so are 6,500 frames of Kirby's Dream Land 3 in the interpreter. |
| P2 | Done 2026-09-11. `tests/snes/rom_baseline.txt`: 64 ROMs pass in the interpreter, among them gilyon cputest-full, cputest-basic and spctest, 23/23 krom CPU and 7/7 krom SPC700 tests, blargg spc_dsp6, spc_timer and spc_mem_access_times, and absindx's SA1RebootTest and SA1RamProtectionTest (hardware-verified). Not listed, with reasons: blargg spc_smp (checks Nintendo's boot ROM), SA1RamProtectionTest (NO SRAM), krom PPU demos whose screenshots do not match exactly (P3). |
| P3 | Done 2026-09-11. PPU ported from ares-performance; 21/21 PPU unit tests; 26 krom PPU demos match their screenshots pixel for pixel at frame 150 and are in the baseline. The others differ by capture quantization, animation or joypad scroll; still to check: RedSpace9BitHDMA and the HiRes/PseudoHiRes PerTileRow frames. |
| P4 | Done 2026-09-11. `tools/snes/recomp_suite.py`: every one of the 88 catalogued test ROMs and the checklist ROM gives the same verdict recompiled as interpreted (88/88), and all run clean in `--differential` (88/88). The checklist ROM: 43/43 tests natively, 99.7 % native. |
| P5 | Done 2026-09-11. Audio: a 30-second windowed run of KDL3 with `--audio-stats` never ran dry (queue mean 54.7 ms, minimum 38.7 ms; WASAPI, 48 kHz). Battery saves: the frontend writes `<id>.sav` (checked with KDL3 and `tests/fixtures/make_snes_sram_rom.py`). The overlay menu opens and closes from Escape, a controller's Guide button and L3, pauses the game, and a setting changed in it survives a restart (`snes_menu_selftest`, synthetic SDL events with the dummy video and audio drivers; not yet tried with a physical controller). |
| P6 | Done 2026-09-11. SA-1 (`snes_sa1.c`; 13/13 SA-1 unit tests; absindx reboot and RAM-protection tests pass). The hub imports, builds (73 s, with a 6,000-frame profile), plays and covers Kirby's Dream Land 3; its `.sav` and `.state1` survive `--rebuild-all` byte for byte. On 6,500 frames of scripted play (intro, title, file select, level map, stage 1) the hub build runs 100.0 % native on both CPUs, `--differential` clean. Hub refactors from "Before the third console" done. |
| P7 | 2026-09-11. A/B: KDL3 built with MSVC and with gcc 14 in the sniper SDK ends 6,500 scripted frames in byte-identical `--dump-state` (including the SA-1 CPU and I-RAM) and frames; the Linux build gives the same state in the SDK, the sniper runtime, and plain Arch Linux. The hub exports KDL3 for all three targets; the Windows and Linux exports run and match. The macOS kit and apps are checked by `.github/workflows/macos.yml` (SNES test apps and a Windows-made SNES kit finished on a Mac), not yet run. |
| P8 | Not started. |

## Mode 7 resolution (2026-10-08)

`snes_ppu_set_mode7_scale()` (2 to 4): a line that shows the mode 7 plane
is drawn a second time, `scale` rows of 256 x `scale` samples, into a
picture beside the PPU. The rule is the one the Nintendo DS and Nintendo 64
follow: the console's line is drawn first and as always, and the larger
picture is only for the display, so frames, state dumps and save states are
what they were at any scale.

- **A sample** is the plane at a fraction of a pixel: `render_mode7()`'s
  matrix and origin with the screen position in scale-ths, in exact 64-bit
  integers. The console rounds three products of the origin down to 64ths;
  the larger picture does not, so that its rows agree with each other. A
  sample at a whole pixel is therefore the console's pixel whenever that
  rounding loses nothing, and within three quarters of a texel otherwise.
- **Between lines.** A game that tilts the plane writes a matrix per line
  (HDMA). The rows between line y and y + 1 use line y's matrix carried on
  by its change since line y - 1, when every term changed by no more than
  an eighth of the largest (and the scroll and centre by no more than 8):
  one surface, not a cut between two. Looking back instead of ahead keeps
  the larger line drawn with the line it belongs to, with that line's VRAM
  and palette.
- **The rest of the line** (sprites, backdrop, windows, colour math) is the
  console's, per pixel: each sample goes through the same priority and
  colour stages as the plane's own pixel did.
- **Where it lives.** A save state is the machine's bytes, so the picture
  is kept in the renderer, one PPU at a time, and `SnesMachine` is the size
  it was: existing save states still load.
- The frame's other rows are filled from the console's at the end of the
  frame, and only for a frame that had a mode 7 line; a game that never
  uses mode 7 pays nothing.

Results (Windows, MSVC): Peter Lemon's Mode7 Perspective, RotZoom and
Mode7HDMA demos give the same console frame and `--dump-state` at 1x and
4x. Perspective, 1,800 frames headless: 0.62 ms a frame at 1x, 1.6 at 2x,
2.5 at 3x, 3.4 at 4x. `snes_ppu_units` has the sample-by-sample check; a
deliberate error of one row in the origin fails it.

Not done: no texture filtering (a magnified texel stays a square); the CRT
filter draws from the console's lines; no commercial mode 7 game has been
looked at; the matrix between lines is a carry-on from the line above, not
an interpolation with the line below.

## Behaviour pinned by the vectors and test ROMs

- In emulation mode a read-modify-write instruction writes the unmodified
  value back in its modify cycle (so the register is written twice).
- WDM is two bytes; its second cycle is internal, the operand is not read.
- The 65816-only instructions (PEA, PEI, PER, PHD, PLD, PLB, JSL, RTL,
  JSR (a,X)) move S as a 16-bit value in emulation mode, then force its high
  byte to $01; the older ones wrap within page 1.
- (dp,X) in emulation mode reads the pointer's high byte from the same page
  as its low byte.
- Interrupts are sampled when the last bus cycle of an instruction starts,
  with the I flag of that moment: CLI lets a pending IRQ in only after the
  next instruction; SEI lets one more in.
- VRAM reads: the latch reloads from the address before it increments, so
  the first word after setting the address is read twice.
- The multiplier/divider step once per CPU cycle; DRAM refresh steps them
  five times.
- The SPC700 boot ROM must start with MOV X,#$EF: software reads $FFC0.

## Known deviations

- **65C816 vectors vs hardware (308 of 5,120,000).** The vectors wrap
  (dp,X)'s pointer high byte into the next page in emulation mode, and wrap
  JSR (a,X)'s pushes within page 1 in emulation mode. gilyon's cputest,
  verified on hardware, shows the opposite (tests 0024, 0027, 0277), as does
  ares. The harness checks those cycles against the hardware rule and counts
  them as deviations.
- **MVN/MVP vectors** stop after 100 bus cycles mid-iteration; the harness runs
  whole iterations and checks the remaining cycles are the next iteration's
  opening fetches. WAI/STP vectors end with an address-less halted cycle.
- **blargg spc_smp "verify IPL ROM"** fails: it checks the CRC of
  Nintendo's boot ROM, which is not used.
- **SA-1 bus conflicts** are not modelled: on hardware the SA-1 waits a cycle
  or two when the S-CPU uses the same ROM, BW-RAM or I-RAM at the same time.
  The SA-1 runs slightly fast when both CPUs hammer the same memory.
- **SA-1 details that differ from ares by a few cycles**: the SA-1 never
  turns an implied instruction's last idle cycle into a PC read before an
  interrupt (2 master clocks per interrupt); and switching the H/V timer from
  linear to H/V mode while H is 1364 or more continues from H mod 1364 rather
  than the start of the next line. An independent review found no other
  differences from ares's SA-1.
- **SA-1 reset** follows the hardware table of absindx's SA1RebootTest (the
  65C816 reset sequence with PC from CRV, A and the arithmetic registers
  kept), not ares, which only reloads PC and DBR.
- **absindx SA1RamProtectionTest (NO SRAM)** fails test 33 (S-CPU BW-RAM
  unprotect): its header declares no BW-RAM, so none is mapped. Real boards
  carry the RAM whatever the header says; no hardware result is published for
  this variant. **SA1VersionCodeTest** (a register dump) always prints
  FAILED by design; its hardware values are published only as images.
