# NES backend: plan

Started 2026-10-05 on branch `feature/nes-backend`, following
[ADDING-A-CONSOLE.md](ADDING-A-CONSOLE.md). This is the working plan for
turning `.nes` ROMs into native executables the way the other consoles do:
recompile the ROM to C, configure with CMake, build with Ninja, launch from
the hub. The backend is the SNES one made smaller: one 8-bit CPU with no
width modes, no coprocessors, a simpler PPU and APU. What it adds is
bank switching on nearly every cartridge, which decides how the recompiler
names code ([P4](#p4-recompiler)).

## Ground rules

1. **License.** Native Console is MIT. Code is copied only from MIT, BSD,
   Apache-2.0, zlib, ISC or CC0 sources and recorded in `THIRD_PARTY.md`.
   ares (ISC) is the port source where one is used. Mesen2, FCEUX, Nestopia
   and puNES (GPL) are not read for code; FCEUX may be run as a separate
   oracle. mstan/nesrecomp is PolyForm Noncommercial: ideas only. Test data
   without a license is used locally only, never committed.
2. **No firmware.** The NES has no BIOS and no boot ROM.
3. **One semantics source.** The interpreter and generated C call the same
   force-inlined handlers `nes_op_XX(c)` in
   `runtimes/runtime-nes/include/nes_cpu_impl.h`. The vectors validate the handlers
   once; the emitter only sequences them.
4. **Guest state is explicit.** Registers and flags live in `NesCpu`; every
   memory access goes through the bus (`nes_bus.c`), one call per CPU cycle.
5. **The bank is part of the code address.** A cartridge maps different ROM
   at the same address over time, so the recompiler's unit is a key: the CPU
   address plus the 8 KiB PRG ROM page mapped there. Because the key names
   the bytes, a compiled entry can never run against the wrong code; what
   the analyzer guesses about banking only decides how much gets compiled.
6. **Interpreter from day one.** Code in RAM, missed indirect targets and
   banks never seen mapped fall back to it. It is also the differential
   oracle.
7. **Evidence layers stay labelled** (`AGENTS.md`): unit tests, differential
   against our own interpreter, and independent evidence (hardware-verified
   test ROMs, vectors). Only the third proves hardware accuracy.
8. **Fail closed.** Missing test data, a timeout, an empty selection, a
   missing `DE B0 61` signature or a ROM that does not finish is a failure.

## What we reuse

| Source | License | Pin | What we take |
|---|---|---|---|
| [SingleStepTests/65x02](https://github.com/SingleStepTests/65x02) `nes6502/` | MIT | `2f6980a` | Per-instruction vectors with bus cycles: 256 opcodes x 10,000, unofficial opcodes included (1.1 GB). |
| [christopherpow/nes-test-roms](https://github.com/christopherpow/nes-test-roms) | none | `95d8f62` | blargg's CPU, PPU, APU and mapper suites, kevtris's nestest. Local test data only. |
| nes15 (in the above) | BSD-2-Clause | `95d8f62` | A small game with source; frame-hash smoke test. |
| [100thCoin/AccuracyCoin](https://github.com/100thCoin/AccuracyCoin) | MIT | `9fc47e0` | 146 accuracy tests in one NROM image, a result byte each at `$0400`-`$0497`. |
| [ares](https://github.com/ares-emulator/ares) `ares/fc/` | ISC | `a776c50` | Behaviour reference and port source for the PPU, the APU and the palette generator. |
| nesdev wiki | reference | | Hardware reference. |
| FCEUX 2.6.6 (user's copy) | GPL-2 | | Separate oracle process only (`-lua` scripts); never read for code. |
| [mstan/nesrecomp](https://github.com/mstan/nesrecomp) | PolyForm Noncommercial | | Ideas only: per-(bank, address) blocks, dispatch-miss logs. No code. |

`tools/fetch_nes_tests.py` downloads the first four at their pins into
`roms/nes/` and writes `MANIFEST.json`.

How each suite reports:

- **blargg, 2010 and later**: `$6000` is `$80` while running, `$81` when
  the test wants the reset button, `$00` on a pass, anything else a failure
  code; `$6001`-`$6003` hold `DE B0 61`; text follows at `$6004`. The single
  ROMs are NROM but expect RAM at `$6000`.
- **blargg, 2005**: a result in zero page `$F8` (1 = pass) and on screen.
- **nestest**: start at `$C000`; `$02`/`$03` are zero on a pass; the log is
  compared line by line.
- **AccuracyCoin**: result bytes in page 4 (bit 0 set = pass).

## Layout

```text
runtimes/runtime-nes/
  include/nes_cpu.h        6502 state
  include/nes_cpu_impl.h   instruction handlers, force-inlined; opcode table
  include/nes_cpu_decode.h opcode classification (analyzer, emitter, tools)
  include/nes_bus.h        bus interface the CPU calls
  include/nes.h            machine: RAM, cartridge, controllers, timing
  include/nes_cart.h       header and mapper state
  include/nes_recomp.h     interface for recompiled code
  src/nes_cpu.c            interpreter, reset, interrupt entry
  src/nes.c                device clocks, interrupts, run loop
  src/nes_bus.c            memory map, I/O, OAM DMA, PPU/APU callbacks
  src/nes_cart.c           iNES / NES 2.0 header, mapper boards
  src/nes_ppu*.c           PPU (dot renderer) and the generated palette
  src/nes_apu*.c           APU and its output stage
  src/nes_recomp.c         program install (SHA-256), hashed lookup, yields
  src/nes_coverage.c       interpreted-code report and traces for --trace
  src/nes_state.c          save states
  src/nes_runner.c         headless runner and --differential
  platform/                SDL2 frontend and overlay menu
  tools/nes_run.c, nes_play.c
recompilers/recompiler-nes/src/        rom, analyzer, emitter, CLI (nesrecomp)
tests/nes/                 SST harness, PPU and APU unit tests, ROM baseline
hub/src/console_nes.cpp    hub backend
tools/fetch_nes_tests.py
tools/nes/                 run_nes_tests.py, recomp_suite.py, asm6502.py,
                           checklist/
```

## Design

### Time

Time is counted in CPU cycles; every bus call is one. The PPU runs three
dots per cycle and the APU one step, but neither is stepped with the CPU.
`nes_sync()` runs each up to the CPU's time in steps that end exactly where
the device could change an output by itself (`nes_ppu_dots_until_event`,
`nes_apu_cycles_until_event`), so every NMI edge and IRQ change is noted
with the moment it happened however late the catch-up runs. The bus does
the same before a register access. `m->next_event` is the next such moment;
the run loop and generated code compare the cycle counter with it between
instructions.

The exception is a cartridge that raises its IRQ from PPU fetches (MMC3):
it cannot say when, so the PPU is followed cycle by cycle while that
interrupt is enabled and the PPU is rendering.

### Interrupts

The 6502 samples its interrupt inputs during the cycle before an
instruction's last. The handlers record that moment (`poll_at`, `poll_i`);
the run loop takes an NMI whose edge arrived by then, or an IRQ whose line
was low by then with I clear. BRK and the IRQ sequence choose their vector
late (`nes_bus_interrupt_vector`), so an NMI that arrives while they push
takes them over.

### Cartridges

`nes_cart.c` keeps a board's state as page tables: four 8 KiB PRG pages,
eight 1 KiB CHR pages, four nametable slots, all as offsets. A register
write rebuilds them; nothing else knows which board is fitted. Boards:
NROM (0), MMC1 (1), UxROM (2), CNROM (3), MMC3 (4), AxROM (7), MMC2 (9),
MMC4 (10), Color Dreams (11), BNROM / NINA-001 (34), GxROM (66),
Camerica (71). By the count of licensed dumps in binjnes's MIT database,
mappers 0-4 and 7 are 88 % of the library (97 % of North American
releases).

NTSC only. PAL and Dendy timing, the Famicom Disk System, Vs. System and
expansion audio are out of scope for now.

## Phases

### P0: research, licenses, test data

Gate: the fetch script runs clean twice and the second run does nothing.

### P1: 6502 core

All 256 opcodes of the 2A03's NMOS 6502 (no decimal mode), unofficial ones
included, with the bus cycles in hardware order. Gate: 100 % of the vectors
in Release, Debug and AddressSanitizer builds; deviations listed below.

### P2: machine and headless runner

Memory map, controllers, OAM DMA, the device clocks, interrupts, mappers.
Runner: `nes_run ROM --frames N`, `--until-idle`, `--blargg` (the `$6000`
protocol), `--expect-ram`, `--dump-frame`, `--dump-state`, `--dump-audio`,
`--keys`, `--auto-input`. Gate: nestest and the CPU suites pass in the
interpreter; `tests/nes/rom_baseline.txt`; every other failing ROM listed
with the phase that owns it.

### P3: video

Dot renderer with the real fetch pipeline, sprite evaluation, sprite-0 hit
and overflow, the `$2002` race. Gate: PPU unit tests; the PPU test ROMs.

### P4: recompiler

- **Keys.** An entry is `page << 16 | PC`, where `page` is the 8 KiB PRG
  ROM page mapped at `PC`. The run loop builds the key from the
  cartridge's current page table before every lookup.
- **Functions stay inside one 8 KiB page.** A branch or jump to another
  page returns to the run loop, which looks the target up under the mapping
  of that moment. Generated code therefore never assumes which bank is
  mapped anywhere but under its own feet.
- **Bank switches.** A write that changes the PRG page table sets
  `NES_ATTENTION_REMAP`; generated code returns after that instruction, so
  the next lookup sees the new bank, including when a routine switches out
  the bank it is running in.
- **Discovery.** From the vectors, and from a profile: `nesrecomp --profile`
  plays the game with the interpreter under a built-in input script and
  records every key at which code was entered. Static exploration follows
  calls and jumps; a target in another page is a guess about which bank is
  there (the same 16/32 KiB bank, the fixed bank, any bank), accepted region
  by region as in the SNES analyzer: a guessed region is dropped if it holds
  BRK, an unofficial opcode, or a jump or call into `$0800`-`$5FFF`, or if it
  straddles accepted code. A wrong guess that survives only costs size, so
  the depth of guessing is not limited (`--speculation N` limits it).
- **Jump tables.** The 6502 dispatches through `LDA lo,X / STA ptr /
  LDA hi,X / STA ptr+1 / JMP (ptr)` or by pushing an address and `RTS`. The
  analyzer notes every indexed ROM load that is stored straight into a RAM
  pointer byte or pushed, pairs the low and high halves (one table of words,
  or separate low and high tables), and reads the table once some `JMP (ptr)`
  through that pointer is known, wherever it is. Each entry is a guessed
  root; the read stops at the first entry that does not point into ROM or
  where the table runs into code.
- Code in RAM is interpreted.
- `--differential`: CPU registers and the cycle counter after every native
  slice; RAM, PRG RAM, CHR RAM, nametables, palette, OAM and the mapper
  every frame.
- Gate: every test ROM gives the same verdict recompiled as interpreted and
  runs clean in `--differential` (`tools/nes/recomp_suite.py`).

### P5: audio, saves, frontend

Battery RAM (`.sav`), SDL2 frontend with the overlay menu (keyboard and
controller, volume, speed, scaling, save states), `nes_play`. Gate: a
30-second windowed run without audio underruns; the overlay menu opens from
a controller alone, pauses, and its settings survive a restart
(`nes_menu_selftest`).

### P6: hub integration and the first game

The user's game is Final Fantasy (USA): MMC1 (SNROM), 256 KiB PRG in
sixteen 16 KiB banks, CHR RAM, 8 KiB battery RAM. Gate: it imports, builds,
plays and gets a cover in the hub; its save survives a rebuild;
`--differential` is clean over 6,000 scripted frames; `--coverage` reports
the native share.

### P7: platforms and export; P8: accuracy and speed

As in ADDING-A-CONSOLE.md. Windows first.

## Status

| Phase | State |
|---|---|
| P0 | Done 2026-10-05. Three suites fetched; the second run is a no-op. |
| P1 | Done 2026-10-05. 2,560,000 / 2,560,000 vectors, ordered bus cycles included, in MSVC Release, Debug and AddressSanitizer builds and a gcc 13 ASan+UBSan build (WSL). The harness also checks the interrupt poll position, `branched` and the decode table on every vector; `nes_cpu_units` covers reset, interrupt entry, NMI hijacking and JAM. |
| P2 | Done 2026-10-06. `tests/nes/rom_baseline.txt`: 158 of the 160 ROMs of nes-test-roms pass in the interpreter (`tools/nes/run_nes_tests.py`, 6 s): nestest's trace on all 8,991 instructions, every blargg CPU suite (instr_test-v5, instr_misc, instr_timing, cpu_timing_test6, branch_timing, cpu_interrupts_v2, cpu_reset, cpu_dummy_reads and writes, cpu_exec_space), the APU suites (apu_test, apu_reset, blargg_apu_2005), DMA (sprdma_and_dmc_dma, dmc_dma_during_read4, read_joy3) and both MMC3 suites for the Sharp chip. The two that fail test the other MMC3 revision (Known deviations). Cartridge unit tests (`nes_cart_units`) cover the header and every board. AccuracyCoin: 131 of 146, with its own baseline. |
| P3 | Done 2026-10-06. 31 PPU unit tests (registers, VBlank/NMI and the `$2002` race, the fetch sequence of a scanline, backgrounds, sprites, sprite 0 hit, overflow, a random-scene comparison against a reference renderer). The PPU test ROMs pass: ppu_vbl_nmi 10/10, vbl_nmi_timing 7/7, blargg_ppu_tests_2005 5/5, sprite_hit_tests 11/11, sprite_overflow_tests 5/5, ppu_open_bus, ppu_read_buffer, oam_read, oam_stress. No frame comparison against a reference emulator has been made; the picture of Final Fantasy and nes15 was checked by eye. |
| P4 | Done 2026-10-06. `tools/nes/recomp_suite.py`: all 161 catalogued ROMs give the same verdict recompiled as interpreted (the same failure where one fails) and all run clean in `--differential`; native share median 100.0 %, minimum 63.5 % (cpu_timing_test6, whose code runs from RAM); 80 s. The checklist ROM (54 tests) passes interpreted and recompiled, 98.1 % native (the rest is its RAM code). |
| P5 | Done 2026-10-06. APU: 27 unit tests; the sample stream is bit-identical between MSVC and gcc (pinned hash). Battery saves: `<id>.sav` (checked by the menu self-test and with Final Fantasy). A 35-second windowed run of Final Fantasy with `--audio-stats` never ran dry (queue mean 59.9 ms, minimum 42.1 ms; WASAPI, 48 kHz) and paced 2,100 frames in 35.6 s. The overlay menu opens and closes from Escape, F10, a controller's Guide button and L3, pauses the game, and a setting changed in it survives a restart (`nes_menu_selftest`, synthetic SDL events with the dummy drivers; not yet tried with a physical controller). The menu and the CRT filter were read back from the real window (`--menu-shot`). Save states resume exactly: `nes_state_resume` on the checklist ROM, and on Final Fantasy a state saved at frame 4,200 by recompiled code or by the interpreter, resumed by either, ends frame 9,000 in the same state and picture as a straight run (`tools/nes/state_resume.py`, 8 runs). Loudness at the default volume lies between two SNES games on the same slider (RMS 61 against 37 and 70 of 32,768). |
| P6 | Done 2026-10-06. The hub imports, builds (12 s, with a 6,000-frame profile), plays and covers Final Fantasy (USA); its `.sav` and `.state1` survive `--rebuild-all` byte for byte. On 9,000 frames of scripted play (intro, party creation, the world map, a battle) the hub build runs 99.9 % native, `--differential` clean. Static analysis alone (no profile) decodes 16,000 instructions through 27 jump tables. |
| P7 | Not started (Windows first). The release table has the NES row and smoke recipe. |
| P8 | Not started. Recompiled code is no faster than the interpreter yet (about 1,000 frames per second either way): the PPU's dot loop and out-of-line handlers dominate. |

## Behaviour pinned by the vectors and test ROMs

Numbers of 2026-10-06: 2,560,000 vectors, 0 failures; 158 of the 160 ROMs
of nes-test-roms pass in the interpreter (`tests/nes/rom_baseline.txt`,
`tools/nes/run_nes_tests.py`); AccuracyCoin 131 of 146
(`tests/nes/accuracycoin_baseline.txt`, `tools/nes/accuracycoin.py`); all
161 ROMs give the same verdict recompiled and run clean in `--differential`
(`tools/nes/recomp_suite.py`).

CPU:

- LXA (`$AB`) is `A = X = operand`: the constant it ORs into A is `$FF`.
  Only that gives the checksums of blargg's instr_test-v5 03-immediate,
  nes_instr_test 02-immediate and blargg_nes_cpu_test5 (`$EE`, the vectors'
  value, and `$00` do not).
- An NMI takes a BRK over when its edge reaches the CPU, with the edge
  detector's setup time, by the end of the BRK's fourth cycle: five clocks
  wide counting the cycle before the BRK, then two clocks in which it waits
  for the handler's first instruction (cpu_interrupts_v2 2-nmi_and_brk).
- SHA, SHX, SHY and TAS store the plain register when a DMA halts the CPU
  in the cycle before the write (AccuracyCoin, SH* error codes 7 to C).

Machine:

- A register read sees the PPU after one of its cycle's three dots, a write
  after two, a PPUMASK write after three; the NMI line needs one dot of
  setup (ppu_vbl_nmi, all ten; vbl_nmi_timing, all seven). `$2002` latches
  the VBlank flag there, but returns the sprite 0 hit and overflow flags as
  they are when the read ends, two dots later (AccuracyCoin, `$2002` flag
  timing, first part).
- DMA halts the CPU on a read cycle only and repeats that read while it
  waits. OAM DMA is 513 cycles, 514 when the halt falls on a get cycle. A
  DMC fetch is 4 cycles when its halt falls on a put cycle and 3 on a get
  cycle; inside OAM DMA it costs 2, or 1 or 3 at the end of the page
  (sprdma_and_dmc_dma, both). The repeated reads have their side effects: a
  `$2007` read is seen two or three extra times, a controller port once
  (dmc_dma_during_read4, all five; read_joy3).
- Get cycles are the odd CPU cycles since power, put cycles the even ones.
  The frame counter takes a `$4017` write, and clears its flag after a
  `$4015` read, in put steps: a write in a put cycle restarts the sequence
  two steps later, in a get cycle three (cpu_interrupts_v2 4-irq_and_dma,
  where the write that restarts the counter without the extra cycle starts
  a 514-cycle OAM DMA; AccuracyCoin, the three Frame Counter tests).
- The controllers' strobe pin copies `$4016` bit 0 when a get cycle gives
  way to a put cycle: a 1 written in a put cycle and overwritten in the
  next cycle never strobes (AccuracyCoin, Controller Strobing).
- DMC: the IRQ of a sample's last byte appears on the DMA's read cycle. A
  sample restarted while the buffer still holds a byte does not reload
  before the memory reader has started, 3 or 4 cycles after the write
  (AccuracyCoin, Delta Modulation Channel, 23 parts).
- An event that falls due during an instruction which then touches another
  device's register is still caught up before the next instruction's
  interrupt poll (4-irq_and_dma: an APU IRQ that rises during
  `STA $4014`).
- Reset: `$4017` is written again with its last value (apu_reset, all six).
  The runner presses reset only after an unbroken tenth of a second of
  `$81` at `$6000`.

MMC3:

- A12 rises at the first sprite pattern fetch, dot 261, with the
  background at `$0000` and sprites at `$1000`; with the tables swapped at
  dot 325 of the line before, and at dot 5 of the pre-render line after
  VBlank. The IRQ reaches the CPU a dot after the rise (mmc3_test_2
  4-scanline_timing, which places it to the dot for scanlines 0, 1 and 239
  in both arrangements).
- The idle dot 0 of a line shows the pattern address of the next tile,
  except on the pre-render line, whose dot 0 follows a VBlank line.
- `$2006` and `$2007` accesses clock the counter too; a `$2006` write
  reaches the address lines three dots later (3-A12_clocking in both
  suites).
- Reaching zero raises the IRQ also when the counter was reloaded to zero
  (5-MMC3, 6.MMC3_rev_B).

## Known deviations

- **6502 vectors vs hardware (4,422 of 2,560,000).** The `$AB` (LXA)
  vectors were recorded with the constant `$EE`; blargg's ROMs, verified on
  his console, need `$FF`. The harness checks that each `$AB` vector is the
  `$EE` formula, compares the core with the `$FF` rule, and counts the
  vectors where the two differ as deviations, never as passes.
- **ANE (`$8B`)** keeps the vectors' constant `$EE`. No hardware-verified
  ROM pins it (blargg leaves it out as unreliable; AccuracyCoin accepts
  several values).
- **mmc3_test_2 6-MMC3_alt and mmc3_irq_tests 5.MMC3_rev_A** fail by
  design: they test the other MMC3 revision, where a reload to zero does
  not raise the IRQ. One of each pair can pass; the Sharp behaviour is the
  one implemented. Owner: P8, with NES 2.0 submapper support, if a game
  needs it.
- **AccuracyCoin, 15 of 146 tests** (all P8):
  - DMC DMA Bus Conflicts (error 2), APU Register Activation (4), Internal
    Data Bus (2): the 2A03 decodes its own registers from the CPU's address
    lines even while a DMA drives the bus, so a DMA read can hit `$4015` or
    a controller port, and `$4015` reads stay on the internal bus. Needs
    the bus split into the CPU's address latch and the DMA's.
  - Explicit DMA Abort (2), Implicit DMA Abort (2): a DMC fetch that is
    cancelled, or shortened to one cycle, by `$4015` in the cycles before
    its halt. The APU takes the byte when it asks for it; this needs the
    request and the delivery as two steps (`nes_apu_dmc_read` split in
    two).
  - `$2002` flag timing (2): the dot at which sprite 0 hit and overflow are
    set, to one dot.
  - `$2004` Stress Test (2), `$2007` Stress Test (2), ALE + Read (2):
    register reads take their value when the read ends and a `$2007` read
    disturbs the PPU's own address for a fetch; the PPU is caught up to one
    point per access.
  - OAM Corruption (2), Sprites On Scanline 0 (2), Stale Sprite Shift
    Regs (3), Frozen OAM2 Increment (2), Misaligned OAM2 Address (4), BG
    Serial In (2): what sprite evaluation and the shift registers do when
    rendering is switched off and on in the middle of a line.
- **The DMC's byte is taken when it is requested**, not on the DMA's read
  cycle. Reads cannot tell (the CPU is halted in between); a write to
  `$4015` or a bank switch in the write cycles that hold a halt off can.
- **The MMC3's A12 filter** is a number of dots (9), not three falling
  edges of M2. The PPU's own fetches only make low stretches of 4 dots or
  of 12 and more, so the two agree for them; they can differ for A12
  toggled through `$2006` within three CPU cycles.
- **SHA, SHX, SHY, TAS with a DMA and an index carry at once**: the stored
  value follows the rule above, the address is computed as without a DMA.
  Not covered by any test.
- **No verdict**: apu_mixer (4) and dmc_tests (4) are judged by ear and
  only have to finish; read_joy3 count_errors and count_errors_fast print
  a count (62 and 18 of 1000 here) and any count passes; MMC1_A12 and
  nes15 are run as smoke tests (still running, a picture). nes15-PAL runs
  at NTSC timing: PAL is out of scope.
- **dmc_dma_during_read4** dma_2007_read and double_2007_read print one of
  several checksums a console can give, depending on how its clocks lined
  up at power-on; the ones produced here are `159A7A8F` and `F018C287`.
- **DMA and the strobe pin have no unit tests of their own**: they are
  pinned by the fetched ROMs (`nes_test_roms_baseline`,
  `nes_accuracycoin_baseline`), which CI does not have.
