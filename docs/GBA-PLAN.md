# Game Boy Advance backend: plan

Started 2026-09-10 on branch `feature/gba-backend`; merged to `main` on
2026-09-11. [ADDING-A-CONSOLE.md](ADDING-A-CONSOLE.md) generalises this plan
for other consoles. Its phases P0-P6 are the ones below, it adds platforms and
export as P7, and it calls the accuracy-and-speed phase P8. This is the working plan
for turning `.gba` ROMs into native executables the same way the Game Boy path
does: recompile the ROM to C, configure with CMake, build with Ninja, launch
from the hub. `GBA.md` describes the hub stub and the target layout; this file
says how we get there, what we reuse from other projects, and how every nuance
in the GBA assembly-to-C checklist gets tested.

## Ground rules

1. **License.** Native Console is MIT. We copy code only from MIT, BSD,
   Apache-2.0, zlib or CC0 sources, and record every port in
   `THIRD_PARTY.md`. MPL-2.0 (mGBA) is used as a separate oracle program, never
   linked into shipped code. GPL (NanoBoyAdvance, FuzzARM, agbcc) and
   unlicensed code (gbadisasm, armwrestler) are run as tools or read as
   behaviour references, never copied. PolyForm Noncommercial (mstan/gbarecomp)
   is read for ideas only.
2. **One semantics source.** The interpreter and the generated C call the same
   force-inlined handlers in `runtime-gba/include/gba_cpu_impl.h` (built on
   `gba_cpu_ops.h`), chosen by the shared classifier in `gba_cpu_decode.h`.
   The per-instruction test vectors validate them once; the emitter only
   sequences them. This is the GBA version of the GB lesson that the emitter
   and interpreter must not drift.
3. **Guest state is explicit.** Registers, banked registers, flags and CPSR
   live in a context struct; every guest memory access goes through the bus.
   No guest value lives in a C local across an instruction that can fault,
   branch or touch I/O. This is what makes the C-level items of the checklist
   (integer width, overflow, aliasing, struct padding, endianness, calling
   conventions, compiler optimisation) hold by construction.
4. **Interpreter from day one.** Code copied to IWRAM/EWRAM, indirect branches
   the analyzer missed, and self-modifying code all fall back to the
   interpreter. The interpreter is also the differential oracle for generated
   code, exactly as on Game Boy.
5. **Evidence layers stay labelled** (from `AGENTS.md`): repository unit tests,
   differential mode against our own interpreter, and independent evidence
   (hardware test ROMs, mGBA). Only the third layer proves hardware accuracy.
6. **Fail closed.** Missing test data, a timeout, or an empty selection is a
   failure, never a pass.

## What we reuse

| Source | License | What we take |
|---|---|---|
| [SingleStepTests/ARM7TDMI](https://github.com/SingleStepTests/ARM7TDMI) | MIT | Per-instruction CPU vectors (44 groups × 20,000). Read directly from the `.json.bin` files, no JSON conversion. |
| [jsmolka/gba-tests](https://github.com/jsmolka/gba-tests) | MIT | arm, thumb, memory, bios, ppu, save ROMs |
| [alyosha-tas/gba-tests](https://github.com/alyosha-tas/gba-tests) | MIT | ~130 timing, prefetch, DMA, IRQ, LDM, FIFO and serial ROMs |
| [nba-emu/hw-test](https://github.com/nba-emu/hw-test) | BSD-3 | 25 focused hardware edge-case ROMs |
| [mgba-emu/suite](https://github.com/mgba-emu/suite) | MIT | Memory, I/O, timing, timer, DMA, carry, shifter, multiply, BIOS-math and video subtests. Built from source with its Docker script (devkitARM). |
| [Cult-of-GBA/BIOS](https://github.com/Cult-of-GBA/BIOS) | MIT | Replacement BIOS image. Runs the BIOS tests, validates our HLE SWIs, and is bundleable, so games can boot without the copyrighted Nintendo BIOS. |
| [JRickey/gba-recomp](https://github.com/JRickey/gba-recomp) | MIT or Apache-2.0 | Design reference for the analyzer and runtime; candidate ports later: MP2K driver detection, RTC/GPIO. `gamedb.sqlite` (CC0) as an optional function-boundary source. |
| [luvdis](https://github.com/aarant/luvdis) | MIT | Independent function-discovery oracle for the analyzer |
| [Capstone](https://github.com/capstone-engine/capstone) | BSD-3 | Independent decoder oracle for the recompiler's disassembly listings |
| [gbadisasm](https://github.com/camthesaxman/gbadisasm) | none | Run as an external cross-check only |
| [mGBA](https://github.com/mgba-emu/mgba) 0.10.5 | MPL-2.0 | Frame, memory and audio oracle. Already at `../tools/mGBA-0.10.5-win64`; its Lua scripting and `socketserver.lua` let a tool read state from a running game. |
| GBATEK (problemkaputt.de) | reference | Hardware reference; first stop for every hardware question |

Nintendo's BIOS and AGS Aging Cartridge are not used or distributed. The
default BIOS is our own HLE (`gba_bios.c`): the vectors and IRQ dispatcher sit
at the Nintendo addresses, so BIOS open-bus reads match, and SWIs run in C. A
user may pass their own BIOS dump (`--bios`) for LLE. The Cult-of-GBA BIOS
turned out not to aim for accuracy (its README says so, and its code layout
gives different open-bus values), so it is only an optional boot screen.

## Layout

```text
runtime-gba/
  include/gba_cpu.h         CPU state, modes, banking, exceptions, pipeline
  include/gba_cpu_decode.h  opcode classification (interpreter, analyzer, emitter)
  include/gba_cpu_impl.h    instruction handlers, force-inlined
  include/gba_cpu_ops.h     ALU, shifter, multiply-carry and load helpers
  include/gba_bus.h         bus interface the CPU calls
  include/gba_recomp.h      interface for recompiled code
  src/gba_cpu*.c            interpreter, banking, exceptions, block transfer
  src/gba.c                 machine, scheduler, video timing, run loop/dispatcher
  src/gba_bus.c             memory map, waitstates, open bus, Flash/SRAM
  src/gba_io.c              I/O register table
  src/gba_timer.c, gba_dma.c, gba_ppu.c, gba_apu.c, gba_eeprom.c
  src/gba_bios.c            HLE BIOS
  src/gba_recomp.c          program install (SHA-256 check) and entry lookup
  src/gba_runner.c          headless runner and --differential
  platform/gba_sdl.cpp      SDL2 frontend and settings menu (Dear ImGui)
  tools/gba_run.c, gba_play.c
recompiler-gba/src/         rom, analyzer, emitter, CLI (gbarecomp)
tests/gba/                  SST harness, EEPROM test, R12 baseline
tools/fetch_gba_tests.py    pinned downloads into roms/gba/ (gitignored)
tools/gba/                  run_gba_tests.py, recomp_suite.py, compare_frame.py, ppm_to_png.py
```

## Phases

Each phase ends with a gate. A phase is done when its gate passes, not when the
code is written.

### P0: test data and plan (this commit)

- `tools/fetch_gba_tests.py` downloads every suite at a pinned commit into
  `roms/gba/<suite>/`, verifies the commit, and writes `roms/gba/MANIFEST.json`.
- Gate: the script runs clean twice (second run is a no-op).

### P1: ARM7TDMI core

- CPU state with the six register banks, CPSR/SPSR, mode switching,
  exceptions (reset, undefined, SWI, IRQ, FIQ).
- Full ARM and Thumb interpreter: data processing with the barrel shifter and
  its carry-out (including the shift-by-32 and RRX cases), PSR transfer,
  multiply and long multiply, single and halfword/signed transfers with
  ARM7TDMI misaligned-access rotation, LDM/STM with all the base-in-list and
  empty-list quirks, SWP, branches, BX interworking, SWI, and undefined
  coprocessor space.
- `tests/gba/sst_arm7tdmi.c`: loads each `.json.bin`, drives the core through
  a transaction-replaying bus, and compares every register, bank, CPSR, SPSR,
  the final R15, and the ordered list of data reads and writes (address, size
  and written data).
- Gate: all 44 files pass 100 %, or each remaining failure is listed in this
  file with the reason (the upstream README already flags illegal R15
  write-back cases). The harness builds and passes at `/O2` and under clang
  `-fsanitize=undefined`.

### P2: machine model and headless runner

- Memory map with mirrors and access widths: BIOS (read protection), EWRAM,
  IWRAM, I/O, palette, VRAM (including the 96 KiB mirror quirk), OAM, ROM
  waitstate regions, SRAM/Flash/EEPROM. 8-bit writes to palette/VRAM/OAM
  behave as on hardware. Open-bus reads.
- WAITCNT waitstates and N/S cycle accounting. The prefetch buffer is
  approximated first and made exact in P7.
- I/O register file described as data (address, width, read mask, write mask,
  side-effect hook) rather than hand-written per register.
- Timers with cascade, DMA 0-3 with every start mode, IRQ/IE/IF/IME, HALT and
  STOP, KEYINPUT/KEYCNT.
- BIOS: HLE for every SWI games use except the sound driver; LLE with a
  user-supplied BIOS. (Planned: check HLE against a user BIOS dump per SWI.)
- `gba-run`: headless runner that executes a ROM in the interpreter for N
  frames and dumps framebuffer and memory. Test-ROM result readers for each
  suite (jsmolka and Alyosha draw results in mode 4; nba and mGBA suite results
  are read from their documented locations).
- Gate: jsmolka arm, thumb, memory and bios pass in the interpreter; nba
  timer, irq and dma pass or are listed as known gaps.

### P3: PPU

- Scanline renderer for modes 0-5: text and affine backgrounds, sprites
  (regular and affine, 1D/2D mapping), windows, blending, mosaic, per-scanline
  latching of affine reference points and DISPCNT.
- Gate: jsmolka ppu ROMs, nba ppu ROMs, mGBA suite video; frame hashes match
  mGBA for the Cult BIOS boot and the jsmolka ppu ROMs.

### P4: recompiler

- `gbarecomp`: shares the decoder tables with the runtime. The analyzer
  tracks ARM/Thumb state per basic block, follows BL/BX, recognises literal
  pools, Thumb BL pairs and jump tables, and resolves `BX Rn` targets loaded
  from literal pools. Entry points come from the reset vector, the IRQ
  handler pointer the game stores at `0x03007FFC`, symbol files (pret
  `.sym`/`.map`), and optionally the CC0 game database.
- Emitter: one C block per instruction calling `gba_cpu_ops.h`; conditions
  as flag tests; PC reads as constants; writes to PC return to the
  dispatcher, which maps guest address plus Thumb bit to a native function
  and falls back to the interpreter.
- RAM-resident code: interpreted, or captured from a trace and verified
  against live RAM on every dispatch (the GB `--use-trace` design).
- Cycle counts summed per block at recompile time; the scheduler runs at
  block edges and at every I/O access.
- Gate: SingleStepTests replayed through generated code for a sample of every
  group; test ROMs from P2 and P3 pass when recompiled; `--differential`
  against the interpreter is clean on the Cult BIOS boot and the test ROMs.

### P5: audio, saves, platform

- PSG channels 1-4 and DirectSound FIFO A/B driven by timer overflow and
  sound DMA; SOUNDBIAS; resampling to the host rate.
- Save type detection from the ROM ID strings (`SRAM_V`, `FLASH_V`,
  `FLASH512_V`, `FLASH1M_V`, `EEPROM_V`) with a small override table; Flash
  command protocol; EEPROM over DMA.
- SDL2 window, input, audio; `--headless`, `--limit-frames`,
  `--dump-frames`, `--screenshot-prefix`, `--save-dir`, like the GB runtime.
- Gate: jsmolka save ROMs; Alyosha FIFO ROMs; audio compared with mGBA on a
  test ROM.

### P6: hub integration and first games

- `gbarecomp_exe` in `hub_settings.h`, `prepare_build()` in
  `console_gba.cpp`, `supported()` flipped on.
- Gate: at least one commercial game the user owns boots and plays in the
  hub, with a differential run clean for 500k instructions.

### P7: accuracy and speed

- Exact prefetch buffer and waitstate timing; the Alyosha prefetcher, timing
  and interaction ROMs and the nba timing ROMs.
- Performance work after correctness, following the GB `NL*` method.

## Checklist coverage

Section numbers refer to the GBA assembly-to-C nuances checklist.

| § | Nuance | Test |
|---|---|---|
| 1, 13 | ARM/Thumb state, BX bit 0, link register | SST `arm_bx`, `thumb_bx`, `thumb_bl_*`; jsmolka arm/thumb; Alyosha `thumb/blx`, `irq/BL_IRQ_R14` |
| 2, 23, 24 | Flags, conditional execution, signed vs unsigned conditions | SST data-processing groups and `thumb_bcc`; mGBA `carry` |
| 18, 19 | Shifts, ROR, shifter carry | SST shift groups; `thumb_lsl_lsr_asr`; mGBA `shifter` |
| 20 | Long multiply | SST `arm_mul_mla`, `arm_mull_mlal`; mGBA `multiply-long` |
| 4, 5 | Access width, misaligned access | SST load/store groups (transaction size and address are compared); jsmolka and mGBA `memory`; Alyosha `LDRSH_misaligned`, `PAL_8_bit_writes`, `VRAM_8_bit_writes` |
| 6, 7, 27, 52 | Hardware registers, open bus, read side effects | mGBA `io-read`; Alyosha `Unused_location_update_bus`; jsmolka `unsafe`; nba `bus/128kb-boundary` |
| 8 | DMA | mGBA `dma`; nba `dma/*`; Alyosha `DMA/*`, `fifo_dma/*` |
| 9, 45 | Interrupts | nba `irq-delay`, `cancel-irq-*`; Alyosha `irq/*`; mGBA `timer-irq` |
| 10, 42, 43, 44 | VBlank waits, waitstates, polling loops, delays | mGBA `timing`; Alyosha `prefetcher/*`, `timing/*`, `Interactions/*`; nba `haltcnt` |
| 11, 39 | BIOS SWIs, decompression | jsmolka `bios`; mGBA `bios-math`; SST `arm_swi`, `thumb_swi`; per-SWI unit tests comparing HLE with the Cult BIOS run in LLE |
| 14 | Stack, LDM/STM | SST `arm_ldm_stm`, `thumb_ldm_stm`, `thumb_push_pop`; Alyosha `LDM/*`, `Push_no_regs`, `Pop_no_regs` |
| 16 | PC-relative loads | SST `thumb_ldr_pc_rel`, `thumb_add_sp_or_pc` for semantics; analyzer tests for literal-pool detection |
| 28, 29 | Palette, VRAM, OAM, tiles | jsmolka `ppu`; nba `ppu/*`; mGBA `video`; frame hashes against mGBA |
| 30 | Sound | Alyosha `fifo_dma/*`; audio comparison against mGBA |
| 17, 37, 38, 41, 47, 48 | Jump tables, code/data mixing, self-modifying code, interpreters | Analyzer unit tests; function boundaries compared with luvdis, gbadisasm and the CC0 game database; RAM-code verification on dispatch |
| 3, 12, 15, 21, 22, 25, 26, 31-36, 46, 50-53 | C-level correctness | Held by ground rules 2 and 3; enforced by building the SST harness and generated test ROMs at `/O2` and with `-fsanitize=undefined` and requiring identical results |
| 59 | Validation method | SST is this method per instruction; `--differential` and mGBA comparisons are it per game |

## Status

| Phase | State |
|---|---|
| P0 | Done 2026-09-10. All seven suites fetched at pinned commits (891 MB SST, ~50 MB ROMs). |
| P1 | Done 2026-09-10; the UBSan run 2026-10-05. 2,250,000 / 2,250,000 SingleStepTests pass in MSVC Release (`/O2`), Debug (`/Od`) and AddressSanitizer builds; `/W4` clean; CTest `gba_sst_arm7tdmi`. Since P2 the harness also checks every instruction fetch and the two-entry prefetch pipeline. Since 2026-10-05 they also all pass under gcc 14, gcc 10 and clang 11 (sniper SDK container, `-O0` to `-O3`), with no `-fsanitize=undefined` reports. Until then gcc builds failed 6,272 `thumb_data_proc` vectors: the Thumb shift-by-register cases did the shift and read its carry-out as two arguments of one call, which C does not order (MSVC and clang shift first, gcc reads first). |
| P2 | Functionally done 2026-09-11. Interpreter passes all 10 jsmolka R12 ROMs (arm, thumb, memory, bios, nes, unsafe, save ×4) and 51/138 of the jsmolka + Alyosha R12 set; CTest `gba_test_roms_baseline` guards the passing set (73 since the P7 timing work). The remaining 87 are cycle-timing tests (prefetch buffer, DMA/IRQ interplay, serial, timer start delay, per-region bus latches), which P7 owns. Built-in HLE BIOS reproduces the Nintendo BIOS open-bus values; Flash 64K/128K and SRAM saves work; EEPROM is P5. |
| P3 | Renderer done 2026-09-11: modes 0-5, text/affine BGs, sprites (regular, affine, OBJ window, semi-transparent, mosaic), windows, blending, green swap. jsmolka ppu ROMs render correctly. The NanoBoyAdvance reference images are photos, so they are checked by eye; `sprite-hmosaic` and `bgx` differ (dot-level latching, P7). |
| P4 | Done for the test ROMs 2026-09-11. `gbarecomp` recompiles all 138 R12 ROMs; every recompiled executable gives the same verdict as the interpreter (138/138) and runs clean in `--differential` lockstep (138/138, registers, CPSR and cycle counter after every native slice, memory every frame). CTest `gba_recomp_jsmolka`; full set via `tools/gba/recomp_suite.py` (~50 s). Not yet validated on a commercial ROM. |
| P5 | Mostly done 2026-09-11. APU (four PSG channels, DirectSound FIFOs fed by timer overflow and FIFO-mode DMA, wave-RAM banking, 32768 Hz output), EEPROM 512 B / 8 KiB (CTest `gba_eeprom_protocol`), SDL2 frontend (window, keyboard and controller, audio via `SDL_AudioStream`, 59.73 Hz pacing, saves, screenshots), `gba_play`. Mixer output stage fixed 2026-09-11 (unsigned PSG, AC-coupled output; CTest `gba_apu_mixer`) and frontend audio kept 80 ms ahead of the device. Cartridge RTC (Seiko S-3511 on the GPIO port) added 2026-09-11: status, date/time, time, reset, writes kept as an offset saved after the battery save; host clock when playing, emulated clock headless (CTest `gba_rtc_protocol`). Not done: audio comparison with mGBA, RTC interrupts, a Pokémon ROM run. |
| P6 | Done 2026-09-11. The hub builds `.gba` ROMs: `gbarecomp --profile` plays the game with the interpreter for 6,000 frames under a built-in input script and records code entry points and RAM images, then `gbarecomp --trace` recompiles with them, then CMake/Ninja. The ROM is embedded in the executable. Covers are captured headless. Gate met with The Minish Cap (USA): hub build from the profile alone runs 99.9 % native, differential clean over 6,000 frames of scripted play. |
| P7 | Speed work done 2026-09-11 (Minish Cap, 6,000 frames headless, Release): 1,107 → 2,085 fps recompiled, 933 → 1,341 fps interpreted. Two thirds of the time was the scanline renderer: text BGs now read one map entry and one tile row per 8 pixels, and layers are composed by painting back to front (frame hashes of Minish Cap and 37 PPU/timing ROMs unchanged; `--hash-frames`). Native entries are found through a hash instead of a binary search. Trace keys no function reaches now start one function per run of code rather than one per instruction (Minish Cap: 705 single-instruction functions → 18, run-loop entries 24.4M → 1.9M). Direct C calls for BL between ROM functions were tried and dropped: they covered under 3 % of dispatches. Cycle timing, same day: the Game Pak prefetch buffer (the Alyosha tests' rules as GBAHawk implements them, run lazily at each cartridge access; `gba_cart_access`), non-sequential opcode fetches after data accesses and internal cycles, timers that start 2 cycles after the enabling write and tick once more after a disable, the 0xFFFF "glitch tick", and prescalers on one free-running clock. R12 ROMs 50 → 73 / 138, identically in the interpreter and recompiled code (138/138 result parity and differential clean); all the prefetcher ROMs pass except `prefetcher_branch_thumb_arm_3`, which passed by accident before and is now 4 cycles short after an ARM-to-Thumb BX. Minish Cap differential clean; the model costs about 14 % (2,085 → 1,792 fps recompiled). `gba_run --trace-pc LO:HI` prints per-instruction cycles for this work. Left: DMA/IRQ interplay, interrupt and halt timing (`irq/*`, `Interactions/*`), serial. |

### How recompiled code works

- `gba_cpu_decode.h` classifies opcodes; `gba_cpu_impl.h` holds the
  handlers as force-inlined functions. The interpreter decodes and calls
  them; generated code calls them with constant opcodes, so the compiler
  folds the decoding away. There is one implementation of every
  instruction.
- Each generated C function covers one analyzer function and can be
  entered at any resume label (function entries, branch and jump-table
  targets, return sites after calls, instructions after SWI or MSR).
- Generated code charges the same fetch and refill cycles as the
  interpreter, and after every instruction yields if an event is due or an
  enabled interrupt is pending (`GBA_RC_YIELD`). The run loop
  (`gba_run_slice`) then handles it and re-enters native code at the next
  resume label, interpreting any instructions in between.
- The ROM is embedded in the program (a Windows resource, `.incbin`
  elsewhere) and must match the SHA-256 compiled in.
- A trace (`--trace-out`, or `gbarecomp --profile`, which plays the game
  with the interpreter) lists code entry points: where interpreted
  execution began after a branch, or where native code returned to the run
  loop. It also carries IWRAM/EWRAM images. Entry points become resume
  labels and analysis seeds; those no function reaches start functions, in
  address order, each covering the code it reaches. The run loop finds
  entries through a hash of the entry table. RAM code is compiled from the
  images and checked against live RAM (`memcmp`, only after a write to a
  code page) before it runs natively; code with no match is interpreted.
- I/O reads catch up on due events first, so status registers are exact in
  the middle of long native runs.

## Rotation and scaling resolution (2026-10-08)

`gba_ppu_set_affine_scale()` (2 to 4): a line that shows a rotated or
scaled background or sprite is drawn a second time, `scale` rows of 240 x
`scale` samples, into a picture beside the machine. As on the DS, the N64
and the SNES, the console's line is drawn first and as always, and the
larger picture is only for the display: frames, frame hashes and save
states are what they were.

- **A background sample** is the layer at a fraction of a pixel: the
  console's reference point and PA/PC with the screen position in
  scale-ths, in exact integers, so a sample at a whole pixel is the
  console's pixel. Down the screen the reference point moves by what it
  moved since the line above (which is PB/PD unless the game rewrote it for
  the line, as a floor in perspective does), and PA/PC by their change,
  where the movement is steady; otherwise by PB/PD as the console would.
- **A sprite sample** is the sprite's own matrix at a fraction of a pixel.
  All sprites of the line are drawn again at the scale, so priorities
  between them hold; ordinary ones come out as their pixels repeated.
- **Composition** (windows, priorities, blending) is the console's, a
  sample at a time. An OBJ window keeps the console's outline.
- A layer drawn one texel to a pixel, square to the screen, and a sprite
  with the identity matrix, are not reasons to draw a line larger: bitmap
  modes and games that set the rotation bit on everything pay nothing.
- The picture is kept in the renderer, one machine at a time, so
  `GbaMachine` and save states are the size they were.

Results (Windows, MSVC): `gba_ppu_large`, 83 checks, sample by sample for
tiled and bitmap layers, a matrix set every line and a sprite at 2x, 3x and
4x; a deliberate error fails 22 of them. The PPU alone on a full-screen
turned layer: 0.12 ms a frame at 1x, 0.95 at 2x, 1.9 at 3x, 2.95 at 4x.
Minish Cap from a save state in a boss fight with scaled sprites,
interpreted and headless: console frame identical at 1x and 4x at frames 60
and 900; every whole-pixel sample of the larger picture is the console's
pixel and 1,391 pixels hold finer detail; 1.1 ms a frame at 1x, 3.9 at 4x.

Not done: one scene of one game has been looked at, through display shots
from the window, and nobody has played with it on; nothing is
filtered (an enlarged texel stays a square); texture packs and the CRT
filter take the console's picture; affine layers with a mosaic.

## ARM7TDMI behaviour pinned by the vectors

The vectors come from NanoBoyAdvance, which is hardware-verified. These are
the non-obvious behaviours they forced; each is implemented in
`runtime-gba/src/gba_cpu_*.c` and must survive any rewrite (the emitter
included):

- **R15 keeps its low bits.** A write to R15 in ARM state (data processing,
  LDR, LDM, MUL, BX to ARM) is not aligned; the fetch ignores the low bits
  but the register keeps them. Thumb aligns with `& ~1` only where the
  instruction does: high-register ADD/MOV, BX, BL suffix and non-empty POP.
- **Empty register list** transfers R15 only and moves the base by 0x40; a
  POP of an empty list does not align the loaded PC.
- **STM with the base in the list** stores the new base unless the base is
  the first register stored.
- **LDM/STM with S** writes the base back into the *user* bank.
- **R15 read in a later cycle is instruction + 12:** register-shifted
  operands, all multiply operands, the SWP base and source, and base
  writeback of LDR/STR/LDRH into R15 (which also refills the pipeline).
- **Multiply carry** is the Booth array's carry-out, not "unchanged"
  (`gba_mul_carry.c`, ported from zaydlang/multiplication-algorithm, zlib).
- **SPSR in user/system mode** reads as CPSR; writes are ignored.
- **MSR forces mode bit 4 to 1** when the control field is written.
- **Misaligned loads:** LDR rotates the aligned word; LDRH rotates the
  aligned halfword by 8; LDRSH at an odd address reads the halfword and
  sign-extends its high byte.
- **Thumb condition 14** (architecturally undefined) branches always.
- Bus addresses in the vectors are unaligned for misaligned accesses; the
  memory system ignores the low bits, so the harness compares aligned
  addresses.

## Known SingleStepTests deviations

None. Record each accepted deviation here with the test group, the case
index, and why our behaviour is right.
