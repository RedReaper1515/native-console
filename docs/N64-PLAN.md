# Nintendo 64 backend: plan

Started 2026-10-06 on branch `claude/n64-console-support-20pcvz`, following
[ADDING-A-CONSOLE.md](ADDING-A-CONSOLE.md). This is the working plan for
turning `.z64`/`.n64`/`.v64` ROMs into native executables the way the SNES
and GameCube paths do: recompile the VR4300 code to C, configure with CMake,
build with Ninja, launch from the hub. It says what we reuse, how the phases
are gated, and which test covers each hardware nuance
([Checklist coverage](#checklist-coverage)).

The first part was done in a Linux cloud session (Ubuntu 24.04, GCC 13.3)
with no commercial ROMs, no MSVC and no GPU. The Windows part started on
2026-10-07 on the owner's machine (MSVC 19.51, a 20-core desktop) with the
owner's copy of The Legend of Zelda: Ocarina of Time (USA, Rev 2); what it
found and changed is under [Windows and the first game](#windows-and-the-first-game).
A result below says which machine it is from.

## Ground rules

1. **License.** Native Console is MIT. Code is copied only from MIT, BSD,
   Apache-2.0, zlib, ISC or CC0 sources and recorded in `THIRD_PARTY.md`.
   ares (ISC) is the behaviour reference for the VR4300, the RSP, the I/O
   interfaces, the PIF and the cartridge, and the source of the ported
   code; parallel-rdp (MIT) is the source of the RDP rasteriser and of the
   VI's filters; of cen64
   (BSD-3-Clause) only the CIC table is taken. Mupen64Plus and its plugins
   (GPL) are not read for code. Test data without a license is used
   locally only, never committed.
2. **No copyrighted firmware.** The PIF boot ROM never runs: `n64_boot.c`
   does what its IPL1 and IPL2 do (copy the header and IPL3 to DMEM, set
   the CIC seed registers and PI timing) and starts the cartridge's own
   IPL3, which is part of the user's ROM. IPL3 then initialises RDRAM
   (`n64_rdram.c`), talks to the PIF (`n64_pif.c`: the boot handshake and
   CIC-NUS-6105's challenge) and loads the game, so whatever it leaves for
   the game to check is there. Boot code we do not recognise (homebrew),
   or `--hle-boot`, skips IPL3 and starts from the state it leaves. The
   PIF's joybus side is high-level.
3. **One semantics source.** The interpreter and generated C call the same
   force-inlined handlers `n64_op_<name>(c, insn)` in
   `runtimes/runtime-n64/include/n64_cpu_impl.h` and `n64_fpu_impl.h`. The vectors
   validate the handlers once; the emitter only sequences them, and the
   instruction word is a constant in generated code, so field extraction
   folds away.
4. **Guest state is explicit.** Registers, HI/LO, COP0, the TLB and the FPU
   live in `N64Cpu`. Loads and stores go through the handlers (a fast path
   for cached and uncached RDRAM in kernel mode, the bus for the rest). No
   guest value lives in a C local across an instruction.
5. **Code is keyed by virtual address and checked by its bytes.** N64 games
   load code from the cartridge into RDRAM (overlays, TLB-mapped segments),
   so a native function is valid only while RDRAM holds the bytes it was
   compiled from. Each function's block is compared with RDRAM before first
   use; stores and DMA that touch a 64-byte code granule clear the check.
6. **Interpreter from day one.** Code a trace never saw, code loaded after
   the profile, and code whose bytes changed fall back to it. It is also
   the differential oracle.
7. **Evidence layers stay labelled** (`AGENTS.md`): unit tests, differential
   against our own interpreter, and independent evidence (hardware-verified
   test ROMs, vectors from a second implementation). Only the third proves
   accuracy.
8. **Fail closed.** Missing test data, a timeout, an empty selection or a ROM
   that does not finish is a failure.

## What we reuse

| Source | License | What we take |
|---|---|---|
| [ares](https://github.com/ares-emulator/ares) 9408cb43 | ISC | Behaviour reference and ported code: VR4300 exceptions, COP0, address translation and TLB, the FPU's unimplemented-operation rules; the RSP scalar unit, vector unit, DMA and the reciprocal tables; MI, VI (timing; the window of `VI::refresh`, that is which lines and columns are drawn and the guard band; and its nearest-neighbour scan-out, kept as the unfiltered path), AI, PI (DMA timing and block quirks), RI, SI; the PIF joybus, controller and Controller Pak, EEPROM; the FlashRAM model; the game database (save type, size, accessories) as data. Rewritten in C. |
| [parallel-rdp-standalone](https://github.com/Themaister/parallel-rdp-standalone) 388d70f5 (upstream parallel-rdp 1cecd042) | MIT | The RDP: the command interface, triangle setup and span walk, TMEM and the texture unit, the colour combiner, the blender, depth, coverage and dither, ported from its GLSL shaders to C (`runtimes/runtime-n64/src/n64_rdp*.c`). The VI stage too (`n64_vi.c`, `n64_vi_luts.c`): fetch with the hidden coverage bits, anti-aliasing, dither filter, divot filter, the fetch bug, bilinear scaling, gamma and gamma dither, from `video_interface.cpp` and `extract_vram.comp`, `vi_fetch.frag`, `vi_divot.frag`, `vi_scale.frag`, `noise.h`. |
| [cen64](https://github.com/n64dev/cen64) `si/cic.c` | BSD-3-Clause | The table of IPL3 CRCs that identifies the CIC chip. |
| [PeterLemon/N64](https://github.com/PeterLemon/N64) (krom) 7085543e | Unlicense | CPU, FPU, RSP, RDP and interface test ROMs. Local test data. |
| [lemmy-64/n64-systemtest](https://github.com/lemmy-64/n64-systemtest) 196f5421 | MIT | Expected values for RSP corner cases, used as data in `tests/n64/n64_rsp_test.c`. The test ROM itself needs a Rust MIPS toolchain to build and is not run yet. |
| [Dillonb/n64-tests](https://github.com/Dillonb/n64-tests) bffe242e | none | Source only; the ROMs need building. Local only. |
| [Unicorn](https://github.com/unicorn-engine/unicorn) (QEMU's MIPS64 R4000) | GPL-2.0 | Run as a tool by `tools/n64/gen_vectors.py` to produce `tests/n64/vectors/vr4300.vectors`; never linked. |
| this repository's SNES runtime | MIT | `n64_sha256.c` and the SDL frontend's structure. |

`tools/fetch_n64_tests.py` downloads every suite at its pinned commit with a
sparse git checkout into `roms/n64/` and writes `MANIFEST.json`.

## Layout

```text
runtimes/runtime-n64/
  include/n64_cpu.h         VR4300 state, COP0, TLB, FPU registers
  include/n64_cpu_decode.h  opcode table and classifier (analyzer, emitter, tools)
  include/n64_cpu_impl.h    integer, memory, branch and COP0 handlers, force-inlined
  include/n64_fpu_impl.h    COP1 handlers on the host FPU in strict mode
  include/n64_bus.h         what the CPU calls for memory
  include/n64.h             the machine: RDRAM, cartridge, devices, scheduler
  include/n64_rsp*.h, n64_rdp*.h   the two coprocessors behind seams
  include/n64_rsp_fast.h    the RSP's vector unit with SSE2 (the plain handlers stay as the reference)
  include/n64_recomp.h      interface for recompiled code
  src/n64_cpu.c             interpreter, exceptions, COP0, translation, TLB
  src/n64.c                 scheduler, interrupts, run loop
  src/n64_bus.c             physical memory map, ISViewer
  src/n64_io.c              MI, VI, AI, PI, RI, SI registers and DMA
  src/n64_vi.c              scan-out to the host frame: window, the VI's filters and scaler
  src/n64_vi_luts.c         the gamma table
  src/n64_cart.c, n64_cartdb.c   header, CIC, SRAM, FlashRAM, game database
  src/n64_pif.c             joybus: controllers, Controller Pak, EEPROM
  src/n64_boot.c            IPL1/IPL2, or the state IPL3 leaves the machine in
  src/n64_rdram.c           the RI and the RDRAM chips' registers
  src/n64_rsp*.c            RSP (from ares)
  src/n64_rdp*.c, n64_rdp_*.h   RDP (from parallel-rdp): commands, TMEM, the pixel pipeline in headers
                            compiled into the rasteriser, a scanline shader for the common case
                            (n64_rdp_quad.h), and the helper threads (n64_rdp_workers.c)
  src/n64_recomp.c          program install (SHA-256), lookup, code checks
  src/n64_coverage.c        interpreted-code report and traces
  src/n64_runner.c          headless runner and --differential
  platform/                 SDL2 frontend and overlay menu
  tools/n64_run.c, n64_play
recompilers/recompiler-n64/src/         image (views of RDRAM), analyzer, emitter, CLI (n64recomp)
tests/n64/                  vectors, CPU/RSP/RDP unit tests, ROM baseline, fixture
tests/fixtures/make_n64_rom.py   repository-owned MIPS assembler and fixture ROM
hub/src/console_n64.cpp     hub backend
tools/fetch_n64_tests.py
tools/n64/                  run_n64_tests.py, recomp_suite.py, compare_rdp_tests.py, try_game.py,
                            gen_vectors.py, gen_cartdb.py
```

## Phases

### P0: research, licenses, test data

Gate: the fetch script runs clean twice and the second run does nothing.

### P1: VR4300

- All MIPS III integer instructions with the VR4300's 64-bit behaviour, the
  delay slot and branch-likely rules, exceptions with EPC/BD in a delay
  slot, COP0 (Count/Compare at half the CPU clock, a deterministic Random,
  the 32-entry TLB with refill and XTLB vectors), and COP1 in both FR modes
  with the unimplemented-operation exception, denormal handling and the
  default NaNs. Instruction timing: one cycle each, plus the multiply and
  divide stalls, the FPU's latencies and one cycle for a branch-likely that
  is not taken. Caches are not modelled.
- Gate: `tests/n64/vectors/vr4300.vectors` (4,521 cases over 180 operations,
  generated from a second implementation) and `n64_cpu_units` (what the
  vectors cannot pin) pass in Release, Debug and AddressSanitizer + UBSan
  builds; deviations listed below.

### P2: machine and headless runner

- RDRAM (8 MiB), the cartridge bus, SRAM, FlashRAM, EEPROM, the Controller
  Pak; MI interrupts; PI, SI, SP and DP DMA; the VI and AI timed in CPU
  cycles; the RSP run alongside the CPU at two thirds of its clock; the RDP
  run synchronously when DPC_END is written; the cartridge's IPL3 run
  after high-level IPL1/IPL2 (CIC 6101-6106), or skipped for unknown boot code.
- Runner: `n64_run ROM --frames N`, `--until-gpr`, `--expect-gpr`,
  `--isviewer`, `--expect-isviewer`, `--fail-isviewer`, `--dump-frame`,
  `--dump-fb`, `--dump-state`, `--dump-audio`, `--keys`, `--auto-input`,
  `--trace-last`.
- Gate: krom's CPU, FPU and RSP tests pass; `tests/n64/rom_baseline.txt`.

### P3: video and the RDP

- The VI scans RDRAM out into a 640-wide host frame. The window (H and V
  start and end, the guard band, interlace) is ares's; the filters and the
  scaler are a C port of parallel-rdp's VI stage, following its shaders
  and deterministic. `vi_raw` keeps the unfiltered scan-out. The RDP is a
  C port of parallel-rdp, bit-exact and deterministic.
- Gate: `n64_rdp` and `n64_vi` unit tests; krom's RDP ROMs compared with
  their reference images (`tools/n64/compare_rdp_tests.py`).

### P4: recompiler

- `n64recomp` profiles the ROM with the interpreter (`--profile`, 3,000
  frames by default), records which code ran, the 4 KiB RDRAM pages it ran
  from and all of RDRAM as the run left it, and explores from those entries
  and the boot entry point by recursive descent: first through the whole
  image, where a function may cross pages and a call is followed into code
  the run never reached, then through the page snapshots the image does
  not already cover (overlays since replaced). Functions are keyed by
  virtual address and carry the RDRAM bytes they were compiled from (a
  block, one range, so a jump far from a function's start begins another);
  the dispatcher runs one only while RDRAM still holds those bytes.
  Functions that are the same instructions from the same bytes at the same
  addresses in several views are compiled once.
- Generated code calls the handlers (the small ones inlined, the large ones
  through `n64_ool_*`), adds cycles and instruction counts in batches
  flushed before anything that can observe them, and yields to the run
  loop at the same sync points the interpreter uses (memory operations,
  COP0, the end of a delay slot), so events and interrupts happen at the
  same instruction in both. JAL, JR and JALR go back to the dispatcher. A
  store into a code granule forces a re-check before the next native call.
- An idle loop (a branch to itself over an empty delay slot) takes the
  turns before the next sync at once, in the run loop and in generated
  code, with the counts stepping would give.
- A game's window records what its interpreter had to run while it is
  played (`<id>.trace`, `n64_coverage_merge_trace`), and the next build
  compiles that too.
- `--differential`: CPU registers and the cycle counter after every native
  slice; RDRAM, DMEM, IMEM and the RSP's registers every frame.
- Gate: every test ROM gives the same verdict recompiled as interpreted and
  runs clean in `--differential` (`tools/n64/recomp_suite.py`); the
  repository's fixture ROM (overlays loaded by DMA, a store into a delay
  slot, a timer interrupt, a return through ERET, an idle loop, a jump into
  a branch left to the interpreter) runs 100 % native and clean, built from
  a profile and built from the trace a first build records
  (`n64_recompile_fixture`).

### P5: audio, saves, frontend

- AI resampled to 48 kHz; battery saves (`<id>.sav`: EEPROM, SRAM or
  FlashRAM, then four Controller Paks); SDL2 frontend with the overlay menu
  (`n64_play`).
- Gate: a 30-second windowed run without audio underruns; the overlay menu
  opens from a controller alone, pauses, and its settings survive a restart.

### P6: hub integration and the first game

- Gate: a game imports, builds, plays and gets a cover in the hub; its save
  survives a rebuild; `--differential` is clean over scripted frames;
  `--coverage` reports the native share. The backend stays
  `supported() == false` until this passes.

### P7: platforms and export; P8: accuracy and speed

As in ADDING-A-CONSOLE.md. P8 items already known: CPU and FPU timing
(krom's TIMING tests), instruction and data caches, the RSP's dual issue and
stalls, RDP timing, interrupts taken between any two instructions rather than
at sync points.

## Checklist coverage

Evidence:

- **vec** — `tests/n64/vectors/vr4300.vectors`, through the interpreter and so
  through the handlers generated code calls.
- **unit** — `n64_cpu_units` (`tests/n64/n64_cpu_test.c`), `n64_rsp`,
  `n64_rdp`.
- **krom** — krom's test ROMs in `tests/n64/rom_baseline.txt`.
- **fx n** — case *n* of the fixture ROM (`tests/fixtures/make_n64_rom.py`),
  run interpreted, recompiled and in `--differential`.

| Nuance | Evidence |
|---|---|
| 32-bit results sign-extended to 64 bits | vec (every 32-bit op); krom CPU |
| ADD/SUB/DADD overflow traps, ADDU does not | vec; fx 6 |
| Division by zero and MIN/-1 results | vec; unit (`test_integer_quirks`); krom DIV, DDIV |
| Shift amounts masked; SRA of a non-canonical 64-bit value | vec; unit |
| Delay slot runs after a taken branch and a jump | vec; fx 2, 3; krom |
| Branch-likely annuls the slot when not taken, and costs a cycle | vec; krom BRANCH tests |
| Exception in a delay slot: EPC is the branch, Cause.BD set | unit (`test_delay_slot_exception`) |
| LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR merges | vec; krom; deviation below |
| Address errors on unaligned loads and stores | vec; krom |
| SYSCALL and BREAK vectors | vec; fx 5 |
| KSEG0/KSEG1 direct mapping; KUSEG/KSSEG/KSEG3 through the TLB | unit (`test_tlb`) |
| TLB refill vector vs general vector; TLB modified; TLBP | unit |
| Count at half the CPU clock, Compare interrupt | unit (`test_timer_and_random`); fx 11 |
| Random stays within Wired..31 | unit |
| FPU unimplemented-operation exception for denormal and NaN inputs | unit (`test_fpu_unimplemented`); krom CP1 |
| FPU rounding modes, flags, cause and enable bits | vec; krom CP1 (ROUND, TRUNC, CEIL, FLOOR, CVT) |
| FR=0: odd registers as the high halves of doubles | unit (`test_fpu_fr0`) |
| CVT.L / CVT.S.L / CVT.D.L | unit (`test_fpu_long`) |
| Fast RDRAM path agrees with the bus path | unit (`test_fast_path`) |
| Code loaded by DMA after boot runs natively once seen | fx 8, 9 (two overlays at the same address) |
| A store into compiled code is seen | fx 10 (patched delay slot) |
| RSP vector unit, accumulators, VCC/VCO/VCE, reciprocals | unit (2,234 checks); krom RSP CP2 |
| RSP loads and stores (all widths, wrap, transposes) | unit; krom RSP LOADSTORE |
| RSP fast paths agree with the plain ones: SSE2 vector unit, decoded IMEM (rewritten by the bus, by DMA, between calls), whole-lane loads and stores | unit (`n64_rsp_fast`, 6.4 million checks against the reference handlers and a plain interpreter); `n64_rsp_reference`, `n64_rsp_fast_reference` on the portable path |
| RDP fills, triangles, textures, combiner, blender, depth | unit (`n64_rdp`, 1,874 checks); krom RDP (`n64_rdp_test_roms`) |
| RDP fast paths and threads agree with the pixel-by-pixel path | unit (`n64_rdp`: 40 generated command lists through the reference path and the fast paths on 1, 4 and 16 threads, RDRAM, hidden bits and state compared; TMEM load plans against the full gather) |
| VI fetch, anti-aliasing, dither filter, divot, fetch bug, bilinear scale, gamma, gamma dither, interlace, window | unit (`n64_vi`, 1,565 checks; 72 random frames against a transcription of the shaders) |

## Status

| Phase | State |
|---|---|
| P0 | Done 2026-10-06. Three suites fetched by sparse checkout at pinned commits; the second run is a no-op. |
| P1 | Done on Linux 2026-10-07. 4,521 / 4,521 vectors (29 counted as the listed LWR deviation) and 62 / 62 `n64_cpu_units` checks in a GCC 13 Release build and a GCC 13 Debug AddressSanitizer + UBSan build. Windows, MSVC Release, the same day: the same counts. |
| P2 | Done on Linux 2026-10-07. 116 krom ROMs pass in the interpreter (`rom_baseline.txt`): CPU (43), CP1 (28), RSP scalar (14) and vector (31). Failing, with reasons below: CPUTIMINGNTSC and CP1TIMINGNTSC (timing, P8), six reserved RSP vector opcodes. Windows: `n64_test_roms_baseline` passes against the same baseline. |
| P3 | Done on Linux 2026-10-07 for what the references can show. RDP ported (1,466 unit checks, clean under ASan+UBSan). `tools/n64/compare_rdp_tests.py` compares 127 krom RDP ROMs with their reference images and holds every count to `tests/n64/rdp_baseline.txt` (`n64_rdp_test_roms`): 37 match exactly, among them every TexturesMaskShiftMirror, TextureCoordinates, AlphaCoverage, CombinerOverflow and TEXRECT_Palette ROM and RDPTex0And1. The rest differ for the reasons under Known deviations; one is ours (LoadTLUT into a 32-bit tile). Windows: `n64_rdp` and `n64_rdp_test_roms` pass against the same baseline (37 exact), and Ocarina of Time's title sequence, menus, intro and first room draw correctly. |
| P4 | Done on Linux 2026-10-07 for the test ROMs. `tools/n64/recomp_suite.py`: all 165 krom ROMs give the same verdict and the same frame recompiled (from static analysis alone, no profile) as interpreted, and run clean in `--differential`, booting through the cartridge's IPL3. Without a profile the code IPL3 copies to low RDRAM (its checksum loop, about 24 million instructions) runs interpreted, so most ROMs run 73.8 % native over 60 frames (100 % with the earlier high-level boot); FPU-heavy fractals 43-50 %. Two JuliaInput fractals enable interrupts with no handler installed and spend their time in IPL3's leftovers at the exception vector, as a console would (0.7 % native; 99.9 % with `--hle-boot`). A profile records all of this (LL_LLD_SC_SCD: 100 % with a profile). The fixture runs clean in `--differential` and, with a profile, enters the interpreter nowhere (`n64_recompile_fixture`). Windows 2026-10-07: `recomp_suite.py` over the fetched krom set, 182 / 182 ROMs the same verdict and frame recompiled as interpreted and clean in `--differential`; the fixture, now 14 cases and three builds, passes. |
| P5 | Gates met on Windows 2026-10-07. A 30-second windowed run of Ocarina of Time (1,800 frames in 30.7 s, WASAPI, 48 kHz) keeps the audio queue at 75 ms on average, never below 45 ms, and never dry. The overlay menu's self-test passes on the game with a real window and audio device (opens from the keyboard and from a controller's Guide or L3, pauses, bindings saved back, the volume survives a restart). Sound is produced at the right rate (30.09 s of audio for 1,800 frames). No physical controller has been tried, and nobody has listened to the sound yet: both are the owner's. |
| P6 | Gates met on Windows 2026-10-07 with Ocarina of Time (USA, Rev 2). It imports through the hub's batch mode (profile, recompile, build), gets its cover (frame 1,800: the title), its save survives a rebuild byte for byte and the rebuilt game shows the saved file, a Windows export unpacks and runs, `--differential` is clean over 6,000 scripted frames, and `--coverage` reports 97.1 % native over 24,000 scripted frames (intro and first room). The window holds full speed with no audio underruns. `supported() == true`. P6's "check what the window shows": `--display-shot` of the window's own output was looked at, the presented frames were not logged. |
| P7 | Not started (Windows first). |
| P8 | Not started. |

## Windows and the first game

2026-10-07, MSVC 19.51 Release on a 20-core desktop, the owner's Ocarina of
Time (USA, Rev 2; CZLE, CIC-NUS-6105, 32 MiB, SRAM). Scripted runs use an
empty `--save-dir`; "scripted" means the runner's `--auto-input`, which
starts a new game.

**What worked untouched.** The MSVC build, the four unit suites and the
test-ROM baselines. The 6105 boot path, which no cloud session could run:
the game boots through its own IPL3, passes its RSP boot check, and runs
its attract sequence, file select, name entry, the whole intro and play in
Link's house (24,000 scripted frames), with the HUD, text boxes, the pause
screen and sound.

**What was wrong, and is fixed.**

- `compare_rdp_tests.py` keyed its baseline by a path with backslashes, so
  on Windows every ROM was "not in the baseline".
- The analyzer put a dispatcher label on a branch another function had
  left to the interpreter; the emitter then wrote a `goto` to a label it
  never defined, and the game did not compile. Fixture case 14.
- Every function was compiled once per page snapshot it appeared in: a
  page is snapshotted again when any byte in it changes. 24,652 functions
  and 279 MB of C for 3,000 frames; 3,876 and 40 MB once identical
  functions are folded.
- The game's idle thread (`b .` at 0x80000810) ran for the rest of every
  frame: 1.5 million instructions a frame, of which about 200,000 do
  anything. Taken at once, with identical counts (fixture case 13).
- With every handler inlined the game's C took 178 s to compile. Calling
  the large ones: 24 s, and the executable 13 MB smaller.
- `--differential` reported a divergence in frame 25 that was a pending
  interrupt bit and nothing else: native code that leaves through a call or
  return leaves the events due at that sync point to the next slice's
  start, and the interpreter had already taken them. The harness now takes
  due events on both machines before it compares.
- `--interpreter` on a game executable dropped the ROM compiled into it.
- Functions were cut at every 4 KiB page end and only code the profile ran
  was compiled; the whole RDRAM image a trace now ends with fixes both for
  code that stays loaded (202,000 instructions compiled, not 137,000; the
  interpreter runs 828,000 instructions in the first 1,800 frames, not
  3,747,000).
- The interpreter's idle skip did not work while coverage was on, which is
  when a profile runs: the hub's profile step took 89 s, now 47 s.

**Numbers** (one machine, other builds running beside most of them; treat
them as within a factor of 1.5).

| | |
|---|---|
| Hub import, first build | 3 min 36 s: profile 1 min 43 s (before the idle fix), recompile 10 s, configure 11 s, compile 1 min 29 s |
| Generated C | 57 MB, 4,880 functions, 202,000 instructions, 115,000 entry points; executable 51 MB, 32 MB of it the ROM |
| Native share | 100.0 % of the first 1,800 frames; 97.1 % of 24,000 scripted frames (4.36 of 4.50 billion instructions run; 31.8 billion idle turns left out) |
| What the interpreter still runs there | actor and scene overlays loaded after the profile's 3,000 frames (0x8039B790..., 0x801F5540...), and functions in the main segment reached only through pointers (0x800649B0...) |
| Learning from play | one 2,400-frame session, then a rebuild: 448,000 interpreted instructions over those frames, not 1,183,000 |
| `--differential` | clean over 6,000 scripted frames (9.0 billion instructions) |
| Host time, first 1,800 frames, interpreter, as merged | 40.7 s: CPU 19.2 s, RSP 5.3 s, RDP 15.4 s, scan-out 0.9 s |
| Window, first native build, 1,800 frames | 35.4 s (85 % of full speed), the audio queue ran dry 351 times |
| Host time, first 1,800 frames, native, at the end of the day | 6.6 s (274 frames a second): CPU 2.3 s, RSP 1.4 s, RDP 2.2 s, scan-out with filters 0.8 s |
| Host time, 24,000 scripted frames, native, at the end of the day | 88 s (272 frames a second, 3.7 ms a frame): CPU 31 s, RSP 16 s, RDP 28 s, scan-out 13 s |
| Window, at the end of the day, 1,800 frames | 30.7 s; audio queue 75 ms on average, 45 ms at least, never dry |
| Hub import at the end of the day | 1 min 33 s: profile 15 s, recompile 2 s, configure 2 s, compile 1 min 12 s; then the cover |

The idle skip took the CPU from 4.7 to about 1.3 ms of a 16.7 ms frame in
compiled code; that left the software RDP (8.6 ms a frame on one thread)
and the RSP interpreter (2.9 ms), which the next three paragraphs are
about. The scripted frames reach only the first room: a busier scene will
cost more than 3.7 ms, and none has been timed.

**The RDP.** One thread, pixel by pixel through function calls: about 25 ms
for each picture the game draws, 8.6 ms a frame. Now the pixel pipeline is
in headers the rasteriser compiles as one loop, fill mode writes runs
straight into RDRAM, a scanline shader handles one- and two-cycle
primitives into 16-bit images with the per-primitive work hoisted and the
combiner's colour channels in one SSE2 register, TMEM load layouts are
remembered, and a primitive of 512 pixels or more whose scanlines touch
disjoint memory is shared between the calling thread and helpers and
joined before the next command is read. Nothing is drawn after the
register write returns, so RDRAM never depends on the thread count:
`n64_rdp` runs generated command lists through the old path and the new
ones on 1, 4 and 16 threads and compares every byte. Over the first 1,800
frames the RDP takes 2.2 s with the default 8 threads and about 6 s with
one, where it took 16 s. Primitives under 512 pixels stay on one thread
(the game sends about 1.8 primitives per register write, and each has to
finish before the write returns); drawing ahead and flushing when RDRAM
is observed would lift that and is not done.

**The RSP.** The interpreter fetched, byte-swapped and decoded every
instruction through nested switches and ran the vector unit as scalar
loops: about 10 ns an instruction, 2.9 ms a frame. Now each IMEM word is
decoded once and kept beside the word (a fetch compares the word, so bus
writes and DMA need no notice), the run loop is threaded code, and the
vector unit uses SSE2 with the old handlers kept as the reference and as
the path for hosts without it: 1.4 s where it was 6.2 to 6.7 s over the
first 1,800 frames on the same loaded machine, with the frames, the state
dump and the audio byte for byte what they were. In those frames the
game's RSP also runs a few reserved encodings (about 0.01 % of
instructions each: SPECIAL functions 0x01, 0x0F, 0x11, 0x12, 0x16,
0x1A-0x1C, 0x1E, 0x28 and opcodes 0x1F, 0x26, 0x2C); why was not looked
into.

**The picture.** The scan-out showed the bare framebuffer: the RDP's
dither pattern as grain, stair-stepped edges, 320 columns doubled to 640.
The game sets VI_CONTROL 0x00013016 (anti-aliasing with resampling, the
dither filter, divot, gamma dither), so the VI's filters were ported from
parallel-rdp the same day (`n64_vi`, 1,565 checks; about 0.6 ms a frame,
the same as the old scan-out). The title, menus, intro and first room were
looked at before and after; text and the HUD stay legible.

**Seen and left alone.** During fades the rightmost framebuffer column
(x = 319) keeps the scene's colour. That is the game: its fade
(`TransitionFade_Draw` in zeldaret/oot, `src/code/z_fbdemo_fade.c`) draws a
one-cycle rectangle from (0, 0) to (width - 1, height - 1), and a one-cycle
rectangle does not include its right and bottom edge. A television's
overscan hid it. No capture from a console was made here.

## Internal resolution (2026-10-08)

The RDP draws a second, larger picture for the display
(`n64_rdp_set_scale`, `n64_set_internal_scale`; 2x and 4x). The rule is the
one the Nintendo DS follows: the machine keeps drawing the console's exact
picture, and the larger one is drawn beside it where only the display reads
it, so nothing a game can observe changes and every existing test is
untouched at any scale.

- **What it is.** parallel-rdp's upscaling, whose factor (`SCALING_LOG2`)
  this port had fixed at 1: the same span setup and pixel pipeline on a
  finer grid, attributes stepped by a fraction of their derivatives, depth
  exact at the console's pixels, the "next pixel" of the pipelined texel a
  whole console pixel on. Rectangles carry
  `TRIANGLE_SETUP_DISABLE_UPSCALING` and are drawn at the console's size.
- **Where it goes.** A shadow of RDRAM with scale x scale copies
  ("slices") of it and of the hidden bits: sample (sx, sy) of the pixel at
  an address is at that address in slice sy * scale + sx. An image
  anywhere in RDRAM, of any width, has its larger twin without anything
  being told about it. A third copy, the reference, holds RDRAM as the RDP
  left it: a pixel whose bytes differ was written by the game, has no
  larger picture, and is shown as the console's until the RDP draws on it
  again.
- **The row code.** `shade_row()` takes the scale, so a triangle the
  console's pass draws by the fast path is drawn larger by it too; the
  rest go through the pixel-by-pixel port.
- **When it is drawn.** Not as primitives come: each is queued with its
  spans at the scale, the RDP state it is drawn from (TMEM and the tiles,
  copied when they change) and the pixels to bring into step, and a queue
  is replayed by bands of eight scanlines, one thread a band, each drawing
  the queue's primitives in order. A queue's primitives are checked as
  they join it so that no two bands share a byte (images that start at the
  same byte with the same row length, or that share none). SYNC_FULL hands
  the queue to a background thread; the scan-out waits for a queue only if
  it touches the rows being shown, which for a game that draws into a back
  buffer it does not. The reference is kept by the RDP's own thread as
  primitives come, so what is in step never depends on when the slices are
  written, and the larger picture is the same on one thread or sixteen.
- **Scan-out.** The VI's filters run on the larger picture's pixels and
  coverage (as parallel-rdp does), shared between threads by lines. A
  display pixel is one framebuffer pixel at most, so a 320-wide picture at
  2x fills 640 x 480 and at 4x 1280 x 960. Rows and columns outside the
  picture's own, which the filters read at its edges, are the console's
  pixels.

Results, Ocarina of Time (USA, rev 2), interpreter, 20-core desktop:

| | 1x | 2x | 4x |
| --- | --- | --- | --- |
| ms a field, 1,500 fields, no frame limit | 4.4 | 5.7 | 10.3 |
| audio buffer ran dry, 1,800 fields in the window | 0 | 0 | 0 |
| console frame and `--dump-state` | | identical to 1x | identical to 1x |

How it got there at 4x: drawn as primitives came, pixel by pixel, 30 ms a
field; with the row code 24; rectangles' slices and the scan-out shared
between threads 12.6, but one field in three carried a whole picture and
the audio buffer ran dry 34 times in 1,800 fields; queued and replayed in
bands 10.6 and 5 times; on the background thread, with the scan-out waiting
only for its own rows, 10.3 and never.

The fast path at a scale was checked against the pixel-by-pixel one on the
game (identical larger pictures at fields 700 and 1,500, at 2x and 4x, on
one thread and eight), and a deliberate error in its colour step fails 28
checks of `n64_rdp`.

Not done: a recompiled game has not been timed at a scale; only one game
has been looked at; the unfiltered scan-out reads the larger picture a
pixel at a time and waits for all of it; textures drawn with rectangles
stay at the console's size.

## Behaviour pinned by the vectors and test ROMs

- A branch-likely that is not taken skips its delay slot and costs one
  extra cycle; the harness stops after the branch's own step.
- Interrupts and device events are taken only at sync points (memory
  operations, COP0 instructions, the end of a delay slot, a not-taken
  branch-likely). Generated code yields at the same points, so the
  interpreter and native code see interrupts at the same instruction.
- The FPU runs on the host FPU with the guest rounding mode and strict
  flags (`-ffp-contract=off -frounding-math`, `/fp:strict`); generated
  projects must keep these options (`N64_STRICT_FP`).
- Count is the cycle count halved; Random is derived from the cycle count,
  so runs are deterministic.
- The TLB powers on with every entry's mask covering nothing that matches
  (a zeroed TLB would match address 0).

## Known deviations

- **LWR upper half (29 vectors).** When LWR does not load a word's top byte,
  the VR4300 leaves the upper 32 bits of rt alone (ares, n64-systemtest);
  QEMU's R4000 model, which made the vectors, sign-extends. The harness
  checks the VR4300 rule and counts these cases.
- **Vectors exclude** the .L conversions (QEMU's R4000 lacks them), FR=0
  register views and out-of-range conversions, which QEMU models
  differently; `n64_cpu_units` covers them against ares's rules instead.
- **RSP reserved vector opcodes.** krom's V073, VACC, VEXTN, VEXTQ, VEXTT and
  VSAC expect different results from n64-systemtest, which we follow (the
  adder still runs, vd is zeroed). Open until checked on hardware.
- **Timing.** krom's CPUTIMINGNTSC and CP1TIMINGNTSC fail: caches and
  pipeline stalls are not modelled (P8).
- **RDP: two departures from parallel-rdp**, both following angrylion and
  each pinned by a unit test: LOD_FRAC is computed in one-cycle mode
  whenever the combiner reads it (parallel-rdp leaves it undefined unless
  texture LOD is on, which left krom's textured triangles transparent),
  and TEXEL1 in the second cycle is the next pixel's TEXEL0.
- **RDP reference images.** Most of krom's 16BPP and 32BPP references are
  emulator scan-outs, not framebuffers: they stretch 237 VI lines to 240,
  compute 1.0 × TEXEL0 as TEXEL0 squared, and leave out the attribute offset
  on some triangle edges; two 32BPP ROMs carry the 16BPP pictures. The
  8BPP InternalPalette reference has colours no framebuffer can hold;
  RDPModeInput draws only after a button press. AlphaCompare (40 pixels)
  differs where hardware dithers with random noise and parallel-rdp with a
  hash; CombinerLongTailConstants differs in its first pixel only.
- **RDP LoadTLUT into a 32-bit tile** is skipped, as in parallel-rdp;
  hardware writes TMEM (3 krom ROMs).
- **VI.** The filters were ported from parallel-rdp's shaders and checked
  against hand-worked values and a second transcription of the same
  shaders, not against a console or parallel-rdp running on a graphics
  card. The window is ares's where parallel-rdp differs: the right guard
  band is kept when H_END is exactly the picture's edge (Ocarina of Time's
  is; parallel-rdp would blank its last seven columns), the left one is
  kept when H_START is before the picture, and an odd V_START starts the
  picture one frame row lower. Gamma dither is parallel-rdp's hash of
  position and frame count where hardware uses noise. Registers are read
  once a field. Memory past RDRAM reads as zero where parallel-rdp wraps;
  ORIGIN 0 is still scanned; interlaced fields are woven. The unfiltered
  path samples Y as ares does, which differs from the filtered path by one
  framebuffer row in interlaced modes whose V_START parity does not match
  the field. Only Ocarina of Time's mode (320 x 240, 16-bit, progressive
  NTSC) has been looked at; the others are covered by unit tests alone.
- **PI DMA alignment.** Some rows of krom's DMAAlignment-PI differ.
- **Boot.** IPL1 and IPL2 are high-level; the PIF accepts IPL3's
  checksum without comparing it with the CIC's. The RDRAM chips' current
  thresholds come from a fixed seed. The 6105 path runs Ocarina of Time
  (Windows, 2026-10-07). With `--hle-boot`, or unknown boot code, IPL3
  does not run, and a 6105 game's anti-piracy checks would see what it did
  not leave.
- **Interrupt latency.** Interrupts wait for the next sync point, a few
  instructions at most.
- **Relocatable code.** A function is compiled for the address a trace saw
  it at. An overlay the game loads at another address (its heap moved) is
  the interpreter's until a trace records it there; nothing matches code
  by its relocations.
- **Underflow.** The flush of tiny results depends on the host FPU in one
  corner (results that round up into the normal range).
