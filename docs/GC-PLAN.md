# GameCube backend: plan

> **Not playable yet.** Games build and may start, but expect wrong
> graphics, missing sound and crashes. The hub marks GameCube games the same
> way.

Started 2026-09-11 on branch `feature/gc-backend`, following
[ADDING-A-CONSOLE.md](ADDING-A-CONSOLE.md). This is the working plan for
turning GameCube disc images (`.gcm`, `.iso`, `.ciso`) and bare `.dol`
executables into native executables the way the Game Boy, GBA and SNES paths
do: recompile the game's PowerPC code to C, configure with CMake, build with
Ninja, launch from the hub.

GameCube is the fourth family and the first one that is not a 2D console.
The differences that drive every decision below:

| | GB/GBC | GBA | SNES | **GameCube** |
|---|---|---|---|---|
| CPU | SM83 8-bit | ARM7TDMI 32-bit | 65C816 + SPC700 | **Gekko: PowerPC 750CXe, 485 MHz, big-endian, paired-single FPU** |
| Code size | 32 KB - 8 MB | 1 - 32 MB | 0.5 - 6 MB | **2 - 8 MB of DOL and REL text, relocated at run time** |
| Media | cartridge, memory-mapped | cartridge, memory-mapped | cartridge, memory-mapped | **1.36 GB DVD read through a DMA interface with a filesystem** |
| Video | fixed tile pipeline | fixed tile pipeline | fixed tile pipeline | **Flipper: a 3D pipeline driven by a command FIFO, 16 TEV stages, 2 MB embedded framebuffer** |
| Audio | 4 channels of logic | 2 PSG + 2 PCM | 8-voice S-DSP | **64-voice DSP running game-supplied microcode** |
| Firmware | none | 16 KB BIOS (HLE) | 64-byte boot ROM (ours) | **encrypted IPL with fonts, plus a DSP ROM (both avoided, see Ground rules)** |

Nothing about the pipeline changes. The recompiler still emits C, the
generated project still builds itself with CMake and SDL2, the runtime still
has an interpreter as its oracle, and the hub still drives all of it.

## Ground rules

1. **License.** Native Console is MIT. Code is copied only from MIT, BSD,
   Apache-2.0, zlib, ISC or CC0 sources and recorded in `THIRD_PARTY.md`.
   The most mature GameCube recompilation work (DolRecomp, ModernGekko) and
   the reference emulator (Dolphin, and its `hwtests` suite) are GPL: they
   are run as tools and read as behaviour references, never copied. See
   [What we reuse](#what-we-reuse).
2. **No copyrighted firmware.** The IPL is never used: `gcrecomp` starts the
   game at the DOL entry point with the machine in the state the apploader
   leaves it - BATs, MSR, and the OS low-memory globals at `0x80000000` - all
   written by us. The IPL fonts (`OSGetFontEncode`, `OSLoadFont`) are served
   from a font we generate. The DSP ROM is not used either: audio is emulated
   at the microcode level (AX, Zelda), the same decision Dolphin's DSP-HLE
   makes. A user's own IPL dump may be accepted through `--ipl` for
   comparison, but it is never required and never shipped.
3. **One semantics source.** The interpreter and the generated C call the
   same force-inlined handlers in `runtimes/runtime-gc/include/gc_cpu_impl.h`. The
   decode classifier in `gc_cpu_decode.h` is generated from a machine-readable
   ISA table (see [What we reuse](#what-we-reuse)) and is shared by the
   interpreter, the analyzer and the emitter.
4. **Guest state is explicit.** GPRs, FPRs (both paired-single halves), CR,
   XER, LR, CTR, FPSCR, MSR, the GQRs and the SPRs live in `GcCpu`. Every
   memory access goes through the bus. No guest value lives in a C local
   across a bus access. Guest memory is stored **big-endian**, exactly as the
   console holds it, and every access byte-swaps on a little-endian host, so
   that code which writes a structure as words and reads it as bytes still
   works.
5. **Interpreter from day one.** It is the differential oracle and the
   fallback for code the analyzer misses: overlays loaded from disc, RELs
   linked at run time, and anything reached only through a function pointer.
6. **Evidence layers stay labelled** (`AGENTS.md`). For GameCube the only
   hardware-verified layer is `dolphin-emu/hwtests`, which was written
   against real hardware. Dolphin itself, and vectors generated under it, are
   a *second implementation*, not hardware. Say which layer a number comes
   from.
7. **Fail closed.** Missing test data, a timeout, an empty selection, a ROM
   that does not finish, or a run that writes no state is a failure.
8. **Two renderers, one reference.** The P7 gate requires byte-identical
   frames between an MSVC build and a gcc build. A GPU-backed renderer cannot
   promise that across drivers. So Flipper gets a deterministic **software
   rasterizer** that is the reference for every test, every differential run,
   every headless frame dump and every cover capture, plus an **OpenGL 3.3
   fast path** for play, which is validated against the reference rasterizer
   rather than against hardware. Headless runs always use the reference.

## Contract deviations

Three rules in [the contract](ADDING-A-CONSOLE.md#the-contract) do not fit a
disc-based 3D console. Each is recorded here because the hub, the exports and
CI have to know.

| Contract rule | GameCube | Why |
|---|---|---|
| "The ROM is compiled into the executable. No ROM, BIOS or data file ships next to the game." | The **code** (`main.dol` and every `.rel`) is embedded, as the contract intends. The **disc content** is repacked into one `ID.gcdata` file that ships next to the executable and is listed in the backend's `install_files()`. | A 1.36 GB `.incbin` or `RCDATA` blob is not something MSVC, `ld` or a release zip can be asked to carry. Decided with the user, 2026-09-11. |
| "The only runtime dependency is SDL2." | SDL2, plus the OpenGL 3.3 context SDL2 itself creates, with function pointers loaded through a vendored MIT loader. No new package dependency, and a build without OpenGL still works by using the reference rasterizer. | Flipper cannot be rasterized in software at playable speed. |
| "Per-instruction CPU test vectors pass." | There is no SingleStepTests set for PowerPC. Vectors are **generated** by a test program we write, run on an oracle (Dolphin, and QEMU's 750 model for the scalar subset), then checked into `tests/gc/vectors/` as our own data. The hardware-verified layer is `hwtests/cputest`. | No public per-instruction vector suite for this CPU exists. See [P1](#p1-gekko-core). |

## What we reuse

| Source | License | What we take |
|---|---|---|
| [encounter/powerpc-rs](https://github.com/encounter/powerpc-rs) (formerly `ppc750cl`) | MIT / Apache-2.0 | `isa.yaml`: a machine-readable definition of the whole PowerPC ISA including the 750CXe paired-single extension. It generates our decode classifier and disassembler tables, so the opcode space is described as data rather than written by hand. |
| [encounter/decomp-toolkit](https://github.com/encounter/decomp-toolkit) | Apache-2.0 | DOL, REL and RSO layout, and its approach to function-boundary discovery, tail-call disambiguation and relocation rebuilding. Also run as a tool (`dtk`), whose signature database identifies Metrowerks and SDK functions. |
| [encounter/aurora](https://github.com/encounter/aurora) | MIT | GX-state-to-shader translation: the TEV stage model, texture format decoding (CMPR, I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4, C8, C14X2) and vertex attribute descriptors. Adapted to our own GL backend; we do not take its WebGPU layer. |
| [hedge-dev/XenonRecomp](https://github.com/hedge-dev/XenonRecomp) | MIT | The shape of a PowerPC-to-C recompiler that already works: context struct layout, how CR and XER updates are emitted, and the function-boundary and jump-table configuration file. Xenon is 64-bit with VMX128, so only the scalar 32-bit parts transfer. |
| [N64Recomp](https://github.com/N64Recomp/N64Recomp) | MIT | Already the model for our recompilers: static translation with a dispatcher and an interpreter fallback. |
| [devkitPro/libogc](https://github.com/devkitPro/libogc) | zlib | The documented SDK-level behaviour of OS, PAD, VI, AI, DVD and CARD: which registers a real program touches, and in what order. Used to write our own test DOLs. Not linked into anything we ship. |
| [dolphin-emu/hwtests](https://github.com/dolphin-emu/hwtests) | GPL-2.0 | **Run as a tool, never copied.** `cputest` (cr, fctiw, fctiwz, fprf, frsp, load, mtspr, nan, ni, pairedmove, reciprocal, rlw, srawix) and `gxtest` (tev, rasterization, clipping, lighting, copyfilter, intensity). Verdicts come out through a weak `OSReport` symbol, which our runner hooks - see [P2](#p2-machine-model-and-headless-runner). This is the hardware-verified layer. |
| [Dolphin](https://github.com/dolphin-emu/dolphin) | GPL-2.0+ | **Run as an oracle, read as a behaviour reference, never copied.** Generates the P1 vectors, and is the frame reference for P3. |
| [ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp), [ModernGekko](https://github.com/ExpansionPak/ModernGekko) | GPL-3.0 | **Behaviour reference only.** The most advanced public GameCube recompiler and runtime. Read for which problems they hit; no code taken. |
| [IBM Gekko RISC Microprocessor User Manual](https://archive.org/stream/GCN_SDK_Documentation/IBM%20Gekko%20RISC%20Microprocessor%20User%20Manual_djvu.txt) | reference | The normative CPU document: paired singles, the GQRs, HID2, the locked cache, the write-gather pipe, and the `fres` and `frsqrte` estimate tables. |
| [YAGCD](https://hitmen.c02.at/files/yagcd/yagcd/) | reference | Hardware register map, disc format, DSP, VI, SI, EXI, DI. |
| [bmdview TEV notes](https://github.com/magcius/bmdview/blob/master/tev.markdown), [amnoid Flipper notes](http://www.amnoid.de/gc/tev.html) | reference | The TEV pipeline described independently of any emulator. |
| [QEMU](https://www.qemu.org/docs/master/system/target-ppc.html) `-cpu 750` | GPL-2.0 | **Run as a tool.** A second oracle for the scalar integer and floating-point subset, independent of Dolphin. It does not model paired singles. |
| binutils `powerpc-eabi-objdump` (devkitPPC) | GPL-3.0 | **Run as a tool.** Differential oracle for our disassembler over the opcode space. |

`tools/fetch_gc_tests.py` downloads every fetched suite at a pinned commit
into `roms/gc/` and writes `MANIFEST.json` with the source and license of
each. GPL suites are built and run locally; they are never committed or
redistributed.

### Deliberately not reused

- **Dolphin's VideoCommon, DSP-HLE and PowerPC core.** GPL. This is the
  single biggest "cut down what we need to make" temptation, and it is the
  one we decline. Everything Dolphin-shaped here is re-derived from the
  hardware documents and validated against `hwtests`.
- **libPorpoise** and other SDK re-implementations (MIT). They replace the
  SDK for *source* ports, where the game's own SDK code has been removed. A
  recompiled DOL contains its own statically linked SDK, and HLE at the
  function level would need per-game symbol identification and would break
  the differential model. GX is emulated at the FIFO, not at the API.
- **RecompCore and similar Dolphin forks.** They run recompiled code inside
  Dolphin. That is the opposite of this project's contract, which is a
  standalone executable with no emulator behind it.

## Layout

```text
runtimes/runtime-gc/
  include/gc_cpu.h          Gekko state: GPR, FPR pairs, CR, XER, FPSCR, MSR, SPRs, GQRs
  include/gc_cpu_impl.h     instruction handlers, force-inlined
  include/gc_cpu_decode.h   generated from isa.yaml; shared by interpreter, analyzer, emitter
  include/gc_bus.h          bus interface: BAT translation, MEM1, locked cache, MMIO
  include/gc.h              machine: memory, interrupts, timebase, run loop
  include/gc_recomp.h       interface for recompiled code
  src/gc_cpu.c              interpreter, exceptions, interrupt entry
  src/gc_fpu.c              paired singles, quantised load/store, fres/frsqrte tables, FPSCR
  src/gc_bus.c              BATs, MEM1 and its uncached alias, locked cache, write-gather pipe
  src/gc_pi.c               processor interface: interrupt cause and mask, FIFO base
  src/gc_vi.c               video interface: timing, fields, XFB scan-out
  src/gc_cp.c               command processor: FIFO, CP/XF/BP register writes
  src/gc_gx_state.c         the Flipper register file as data (CP, XF, BP)
  src/gc_gx_raster.c        reference software rasterizer: transform, clip, raster, TEV, EFB
  src/gc_gx_tex.c           texture decode and the texture cache
  src/gc_pe.c               pixel engine: EFB, copy filter, EFB-to-XFB copies
  src/gc_dsp.c              DSP interface, mailbox, ARAM DMA
  src/gc_ax.c               AX and Zelda microcode, 64-voice mixer, ADPCM
  src/gc_ai.c               audio interface: streaming and the 32/48 kHz sample clock
  src/gc_di.c               DVD interface: commands, DMA, timing
  src/gc_disc.c             the ID.gcdata archive: FST lookup, decompression, read scheduling
  src/gc_si.c               serial interface: controller polling, rumble
  src/gc_exi.c              EXI: memory card, and the console sink test DOLs print through
  src/gc_card.c             memory card image, block allocation, CARD API behaviour
  src/gc_os.c               low-memory OS globals, our font, reset and exception vectors
  src/gc_recomp.c           program install (SHA-256), dispatcher, RAM-code checks
  src/gc_state.c            save states
  src/gc_runner.c           headless runner, --differential, the OSReport hook
  platform/gc_sdl.c         SDL2 frontend and the overlay menu
  platform/gc_gl.c          OpenGL 3.3 fast path, shader generation from GX state
  tools/gc_run.c            headless interpreter runner
  tools/gc_play.c           interpreter-only player
recompilers/recompiler-gc/
  src/dol.c                 DOL and REL parsing, relocation, section layout
  src/disc.c                GCM/ISO/CISO reading, FST walk, ID.gcdata packing
  src/analyzer.c            function discovery, jump tables, REL entry points, traces
  src/emitter.c             one C block per guest instruction
  src/main.c                CLI
  data/isa.yaml             from encounter/powerpc-rs (MIT/Apache-2.0)
tests/gc/                   vector harness, device unit tests, ROM baseline
tools/gc/                   run_gc_tests.py, recomp_suite.py, frame tools, the vector generator
```

## Phases

### P0: research, licenses, test data

- This document, with the nuance table below.
- Licenses checked with `gh api repos/OWNER/REPO/license` for every candidate.
  The result: the mature GameCube prior art is GPL and stays a reference,
  while the reusable permissive sources are `powerpc-rs` (the ISA table),
  `decomp-toolkit` (DOL/REL analysis), `aurora` (GX to shaders),
  `XenonRecomp` (PowerPC to C) and `libogc` (SDK behaviour).
- `tools/fetch_gc_tests.py`, `roms/gc/MANIFEST.json`, `.gitignore` entries.
- **Gate:** the fetch script runs clean twice and the second run does nothing.

### P1: Gekko core

The CPU is the one part with no ready-made vectors, so the vectors are built
first.

- Generate `gc_cpu_decode.h` from `isa.yaml` with `tools/gc/gen_decode.py`.
  Validate the disassembler against `powerpc-eabi-objdump` over a sampled
  sweep of the 32-bit opcode space.
- Write `tests/gc/vectors/gen/`: a self-checking program, built with
  devkitPPC, that runs every instruction form with pseudorandom inputs and
  reports the resulting state through `OSReport`. Capture its output under
  Dolphin, and independently under QEMU `-cpu 750` for the scalar subset, to
  produce the checked-in vector files. Where the two oracles disagree, the
  manual decides and the case is recorded in
  [Known deviations](#known-deviations).
- Implement the interpreter against those vectors: integer, rotate and mask,
  compare, CR logic, branches, loads and stores including the multiple and
  string forms, SPR access, and the whole FPU - single and double, paired
  singles, quantised load/store, `fres` and `frsqrte` from the manual's
  tables, the FPSCR exception and rounding bits, and non-IEEE mode.
- **Gate:** 100 % of vectors pass in Release, Debug, ASan and a UBSan build,
  and every deviation is listed with its case and reason.

  `hwtests/cputest` moved to P2: its programs are built against libogc and
  start by bringing up the video interface, so running one needs the machine
  model, not just the CPU.

### P2: machine model and headless runner

- BAT-based address translation. Games set BATs and do not use page tables; a
  page-table walk is implemented but expected to stay unused - reaching it is
  logged as an error, not guessed at. MEM1 is 24 MB at `0x80000000`, its
  uncached alias at `0xC0000000`, the locked cache at `0xE0000000`, and MMIO
  at `0xCC000000`.
- The I/O register file is data (address, width, read mask, write mask,
  side-effect hook), not hand-written per register: CP `0xCC000000`, PE
  `0xCC001000`, VI `0xCC002000`, PI `0xCC003000`, MI `0xCC004000`, DSP
  `0xCC005000`, DI `0xCC006000`, SI `0xCC006400`, EXI `0xCC006800`, AI
  `0xCC006C00`, and the write-gather pipe at `0xCC008000`.
- Timebase and decrementer at bus/4 = 40.5 MHz, the PI interrupt cause and
  mask registers, and the external-interrupt path into the CPU.
- The DVD interface and `gc_disc.c`, so a program can read its own files.
- `gc_run`: N frames, dump frame, dump state, scripted input. It hooks the
  `OSReport` symbol, so `hwtests` verdicts and any `OSReport` from a test DOL
  arrive on stdout, and it serves an EXI console sink for programs that print
  that way instead.
- `tools/gc/run_gc_tests.py` and a baseline; CTest `gc_hwtests_baseline`.
- `tests/gc/programs/gc_memtest.c`, `gc_timetest.c` and `gc_disctest.c`,
  ours, for everything a CPU suite cannot see.
- **Gate:** `hwtests/cputest` and the memory and timing programs pass in the
  interpreter; every other failing program is listed with the phase that owns
  it.

### P3: video

The biggest phase. Flipper is a fixed-function 3D pipeline driven by a command
FIFO, and `gxtest` checks it at the pixel level.

- The FIFO: write-gather pipe into the CP FIFO, command parsing, CP/XF/BP
  register writes, display lists. **Done** - see
  [The command stream](#the-command-stream).
- Transform: position, normal and texture matrices, the projection matrix,
  the viewport, lighting (up to 8 lights), texture coordinate generation.
  **Done** except emboss generation - see
  [The lights](#the-lights-and-the-coordinate-generators).
- Clipping against the guard band, backface culling, the scissor. **Done**
  except for the guard band itself and for a triangle crossing the eye
  plane, which is counted rather than drawn wrong - see
  [The clipper](#the-clipper-and-arithmetic-that-is-not-ieee).
- Rasterization exactly as `gxtest/rasterization.cpp` checks it, including
  the sample pattern and the pixel centre. The sample point is pinned to
  within 10^-5 of a pixel, just above 7/12, so coverage cannot be snapped to
  a sub-pixel grid. **Done** - see [The rasteriser](#the-rasteriser).
- TEV: 16 stages, the colour and alpha combiners, konstant selection,
  indirect textures, fog, alpha compare, Z compare before and after texturing,
  blending, dithering. **The combiners, the konstants and the swap tables
  are done** (docs/gc-tev.md); indirect textures, fog, alpha compare,
  blending and dithering are done - see
  [The output stage](#the-output-stage-and-a-dither-that-should-not-fire) -
  and indirect textures and fog are not.
- Textures: every format, mipmaps, wrap modes, filtering, and the texture
  cache including EFB-to-texture copies. **The formats, the tiling, the wrap
  modes and the palettes are done** - see
  [Textures](#textures-and-the-one-part-with-no-hardware-evidence);
  filtering, mipmaps and the cache are not.
- The EFB (2 MB, up to 640x528) and the pixel engine: copy filter, gamma,
  intensity copies, downsampling to the XFB. **The framebuffer, the address
  window, the copy filter, gamma, the intensity conversion and the tiled
  RGBA8 destination are done** - see
  [The framebuffer](#the-framebuffer-and-getting-pixels-out-of-it).
  Remaining: the other destination formats, and the copy to the display
  buffer.
- VI: fields, interlace, the anti-flicker filter, and scan-out of the XFB
  into our framebuffer. **The copy to the display buffer and the YUV
  conversion are done** (docs/gc-video-out.md, and
  [Getting a frame out](#getting-a-frame-out)); the interface's own timing,
  the fields and the scaler are not.
- Then the OpenGL 3.3 fast path, generating a shader per TEV configuration,
  validated against the reference rasterizer frame by frame.
- **Gate:** the `gxtest` programs pass against the reference rasterizer;
  frames match Dolphin's software renderer within the tolerance in
  `tools/gc/compare_frame.py`; the GL path matches the reference rasterizer
  across the whole test set.
- **Gate: met.** All three parts.
  - The `gxtest` programs pass against the reference rasteriser, at the
    standing floor recorded in `tests/gc/hwtest_baseline.txt`.
  - `gc_drawtest`'s frame differs from Dolphin's software renderer in no
    pixel by more than two, which is what a round trip through YCbCr costs
    (see [Comparing a frame against a reference](#comparing-a-frame-against-a-reference)).
  - The OpenGL path agrees with the reference rasteriser on **every**
    program in `build/gc-programs`, frames and verdict lines both
    (`tools/gc/compare_gl.py`, and [The OpenGL path](#the-opengl-path)).
- Smaller gaps, each marked where it would go rather than silently absent:
  the dynamic S- and T-type indirect matrices (no source states their
  coefficients; counted so a game leaning on one is visible), indirect
  coordinate chaining, anisotropic filtering and the bias clamp, and - on
  the OpenGL path only - blending and the logic operations, which no guest
  program exercises yet and which the generator refuses rather than
  approximates.

### P4: recompiler

- `gcrecomp ROM -o DIR --name ID`:
  - **input**: read `.gcm`, `.iso`, `.ciso` or a bare `.dol`, extract
    `main.dol` and every `.rel`, and pack the files the game needs into
    `ID.gcdata`.
  - **analyzer**: entry points from the DOL entry, the exception vectors,
    REL prolog/epilog/unresolved pointers, jump tables, relocation targets,
    `dtk` signatures, and traces.
  - **emitter**: one C block per guest instruction, calling the shared
    handlers. The generated project compiles them with strict floating
    point, for the reason in [The vector pipeline](#the-vector-pipeline):
    a contracted multiply-add gives different numbers.
  - **dispatcher**: guest address to native function, with an interpreter
    fallback.
  - **resume labels** at function entries, branch targets, return sites -
    and, per the playbook lesson, at every instruction that can yield.
- RELs are linked at run time, so a module's code is recompiled per module
  and installed when the module is linked. A module whose relocated bytes do
  not match what was recompiled falls back to the interpreter.
- `--differential`: registers, FPRs, FPSCR and the timebase after every
  native slice; memory every frame.
- **Gate:** every test program gives the same verdict recompiled as
  interpreted, and runs clean under `--differential`.

### P5: audio, saves, frontend

- DSP interface and ARAM DMA; the AX and Zelda microcodes with the 64-voice
  mixer, ADPCM decoding and the sample-rate converter; AI streaming.
- Memory card: a card image next to the executable, the CARD API's block
  allocation and file directory, formatted by us.
- SDL2 frontend, and the [overlay menu](ADDING-A-CONSOLE.md#overlay-menu)
  adapted from `runtimes/runtime-snes/platform/snes_sdl.cpp`, with the same
  `--menu-selftest` gate.
- **Gate:** the audio unit tests pass; a 30-second windowed run reports no
  underruns; a memory card survives a write, a restart and a rebuild; the
  overlay menu opens from a controller alone, pauses the game, and its
  settings survive a restart.

### P6: hub integration and the first game

- `hub/src/console_gc.cpp`, with `install_files()` returning `ID.gcdata` and
  `is_persistence_file()` covering the memory card image and the menu's files.
- A profiling generate step, as GBA needed: DOL code reached only through
  function pointers, and RELs loaded from disc, are invisible to static
  analysis.
- **Gate:** a commercial game the user owns imports, builds, plays, gets a
  cover, keeps its memory card across a rebuild, is clean under
  `--differential` for 6,000+ frames, and reports its native share.

**Where it stands (2026-09-13), with Shrek 2 (USA):**

- **Imports, builds and plays** through the hub (`console_gc.cpp`, PR #20).
  A batch build from a fresh data directory now takes 30 seconds rather
  than 12 m 41 s (below, and "How long a game takes to build"):

  | hub step | main | now |
  |---|---|---|
  | starting the build script | 1.1 s | 1.1 s |
  | recompiling the disc's executable to C | 0.2 s | 0.3 s |
  | configuring native project | 1.4 s | 1.4 s |
  | compiling native executable | 12 m 37 s | 26.5 s |
  | installing | 0.4 s | 0.0 s |
  | compressing | 0.7 s | 0.6 s |
  | **total** | **12 m 41 s** | **29.8 s** |

- **Gets a cover.** The generated program's headless runner boots a disc the
  way the window does - the disc named with `--disc`, or `<id>.gcdata` beside
  the executable, through the disc's own apploader - and writes the picture
  on screen with `--dump-frame`. A test program (an ELF) is still loaded
  directly with the disc in the drive, so the recompiled-vs-`--interpret`
  gate is unchanged. The runner inserts no memory card. The hub's cover
  setting is tuned for cartridges (600 frames, ten seconds); a disc spends
  its first minute loading and on logos, so a GameCube game runs five times
  as far - 3,000 fields, Shrek 2's title menu, about three minutes headless
  after the build. 600 fields would be the DreamWorks logo, 1,800 Activision's,
  2,400 the legal screen.
- **Keeps its memory card across a rebuild**: `shrek_2.card` survived
  `--rebuild-all` (31 s).
- **One copy of the disc.** The library keeps the disc in `rom/`; the build
  installs `<id>.gcdata` as a hard link to it where the volume allows
  (`platform::link_or_copy_file`), a copy otherwise. A hard link is a whole
  file, so the game folder stays self-contained for Steam and for moving it,
  and 1.25 GB is on disk once rather than twice.
- **The generated project relocates.** `gcrecomp` copies `runtimes/runtime-gc`
  (sources, headers, frontend, and Dear ImGui into `runtime-gc/vendor/imgui`)
  into the project and finds the runtime beside itself, two levels up, or in
  the checkout it was built from, as `snesrecomp` does. Hub export for
  **Windows** works: the zip carries the executable, SDL2.dll and the disc
  (1.03 GB), and the unpacked copy boots the game from another directory.
  **Linux and macOS** exports are refused with "GameCube games cannot be
  exported for this target yet" (`ConsoleBackend::export_problem`): neither
  packs the disc, and the macOS kit needs a bundle script this runtime does
  not have. That is P7.
- **Clean under `--differential` for 6,300 fields (2026-09-13), into
  gameplay.** `G3RE52 --disc ... --frames 6300 --auto-input --differential
  --coverage`: "the recompiled and interpreted machines agree at the end of
  every one of 6300 fields", 51,081,030,001 instructions, 72 minutes on a
  busy machine. The built-in script gets past the logos, the title, New
  Game, "Continue without saving?" and the storybook intro (A skips it); by
  field 1,800 Shrek is in the swamp, walking and using his buttons, to
  the end of the run. What it took to get there is below ("Into the game").
- **Native share (2026-09-13).** Over that run, without a trace: **96.951 %
  by instructions** (49,523,591,388 recompiled, 1,557,438,613 interpreted)
  and **40.376 % by address** (64,536 of 159,839 addresses that ran were
  recompiled at least once; 95,303 only ever interpreted). The instruction
  share is high because the hot loops are reachable by flow; the address
  share is low because most of a game is called through pointers. With a
  trace the picture changes completely - see the profiling step below.
- **Profiling (2026-09-13): the cover run writes a trace, and builds use it.**
  `gcrecomp --trace FILE` makes every address a run's `--trace-out` saw the
  interpreter entered at a root. Measured on Shrek 2 over 3,000 fields:

  | run | trace | blocks | native share (instructions) | addresses interpreted |
  |---|---|---|---|---|
  | no input | none | 22,839 | 99.758 % | 44,902 |
  | no input | that run's | 34,683 | 99.995 % | 172 |
  | `--auto-input` | none | 22,839 | 97.885 % | 96,082 |
  | `--auto-input` | that run's | 56,436 | 99.995 % | 172 |

  Frames 600, 1,200 and 3,700 are byte-identical with and without the trace.
  What is left interpreted is the OS's exception vectors in low memory
  (0x80000500, 0x80000c00, 0x80000300), which the OS writes at run time and
  are not in the executable, and a handful of addresses.
  Over the whole scripted 6,300-field run, rebuilt with the gate run's own
  trace (57,815 blocks): **99.996 % by instructions**, 2,096,531 of
  51,081,030,000 interpreted, 172 addresses; its field 6,300 is
  byte-identical to the differential run's.

  A profiling step *before* the build, as GBA has, would need a built game or
  the interpreter (a sixth of the speed) and minutes of either: 3,000 fields
  are 2.5 minutes recompiled with no input, and a scripted run into gameplay
  is slower still because the reference rasteriser is. So the hub adds no
  step. The cover capture already runs the game 3,000 fields, and now writes
  `<game_dir>/profile/cover.trace`; every build passes each `*.trace` in
  `profile/` to `gcrecomp` (a longer play's `--trace-out` dropped there is
  used too). The first build has no trace, a rebuild has. Hub batch build in
  a fresh data directory, then `--rebuild-all` with the cover's trace (the
  scripted differential running on the same machine throughout):

  | hub step | first build | rebuild with trace |
  |---|---|---|
  | starting the build script | 1.1 s | 1.1 s |
  | recompiling the disc's executable to C | 0.2 s | 0.3 s |
  | configuring native project | 1.4 s | 1.4 s |
  | compiling native executable | 32.6 s | 37.5 s |
  | installing | 0.0 s | 0.0 s |
  | compressing | 0.6 s | 0.8 s |
  | **total** | **35.9 s** | **41.1 s** |

  `shrek_2.card` survived the rebuild byte for byte.

  Such a rebuild ran slower until the emitter learned to recognise a wait
  loop the trace's entry points had cut into blocks. See "Wait loops a trace
  had cut into pieces" under the recompiler.
- **RELs:** Shrek 2 has none (its file system holds `.dsp`, `.gci`, `.cib`,
  `.npb` and the like, no `.rel`, `.dol` or `.elf`), so nothing is loaded as
  code from the disc. A game that has them still needs the per-module work
  under "The recompiler / Not done yet".

**What `--differential` assumes**, since the dispatcher is about to change
(direct block chaining):

- The machines are compared only at an **instruction count**, at the end of
  each field: the recompiled machine runs a field however its code runs
  (block by block or chained) and stops a few instructions past the field
  boundary; the interpreter is stepped to that exact count. Nothing depends
  on where blocks end.
- At that count the interpreter has not yet looked at what is due before the
  next instruction, and recompiled code may have (an interrupt taken on the
  way out of a block, which moves the program counter without retiring an
  instruction). When the program counters differ at an equal count, the
  interpreter gets that look (`gc_machine_before`) before anything is
  compared.
- Both machines are brought up to date (beam, clocks) at the count before
  devices are compared; that is a function of the count.
- **The two machines share no mutable state.** The expansion bus's device
  select and command word were file statics in `gc_io.c` and are now in
  `GcMachine`. What is still file-static is scratch rewritten on every use
  (rasteriser rows, the TEV plan), a cache keyed by its whole input (the
  compressed-texture palette), or the same for both (OS hooks from the
  executable's symbols).
- Input is set between fields, identically on both, exactly as a frontend
  sets it.
- Compared: every architectural processor register and the instruction
  count; main memory, ARAM and both EFB planes; every I/O register (named in
  the report); the graphics register files, ring pointers and draw counters;
  interrupts, the beam, the audio and DSP state, the drive.
- Finding the block after a divergence steps the block table the way
  today's dispatcher does (`gc_runner.c`, `locate_pass`). It is a diagnostic:
  if the real dispatcher runs blocks some other way, the field may come out
  clean stepped one block at a time, and the report says exactly that.
  Tried with a block deliberately corrupting two bytes: the field-end
  comparison named the bytes, and the second and third passes named the
  block and the 32 before it.

**The pieces**, all in the generated program's headless runner
(`runtimes/runtime-gc/src/gc_runner.c`, moved out of the text `gcrecomp` writes):
`--differential`; `--coverage`, which counts interpreted instructions in
`gc_machine_step` (the one path every interpreted instruction takes, so it
needs no hook in blocks or the dispatcher), makes recompiled the rest, and
gets the share by address from the interpreted machine beside it;
`--trace-out FILE`; `--input SCRIPT` (a controller state per field,
`FIELD BUTTONS [SX SY [CX CY [L R]]]`) and `--auto-input` (a built-in script:
START and A presses every second from field 600 for twenty presses, then a
cycle of stick directions and the other buttons, never START); and
`--dump-frame-every N`. Without them the run is the one call it always was.

### Into the game: two machine bugs (2026-09-13)

With scripted input the game halted at field 1,393, a few fields into the
storybook: the program counter sat in `PPCHalt` with interrupts off. The
recompiled and interpreted machines agreed all the way there, so it was the
machine. The fault was a data storage exception inside the heap's free,
reading a block header through a pointer made of garbage, read from pages of
the game's own virtual memory (the region at 0x7E000000 that "And pages"
describes).

1. **A store never marked its page changed.** The page table walk sets the
   referenced bit on a hit and the changed bit on a store, but `translate()`
   passed "not a store" for every access. The virtual memory evidently
   writes a page back to ARAM only when it is marked changed (inferred from
   the transfers, not from its code), so pages it had just filled were
   dropped. Fixed; after it, the storybook turned a page and the game
   crashed the same way at field 2,280.
2. **ARAM wrapped at 16 MB.** The SDK probes past 16 MB to size the
   expansion. A wrap answers the probe with memory that keeps what was
   written, so the SDK concluded an expansion was fitted, handed out audio
   memory above 16 MB, and the virtual memory paged into it - on top of the
   sound data at the bottom of ARAM. The transfer address now decodes to
   64 MB: the first 16 MB are ARAM, the rest the expansion port, which with
   nothing in it reads as zeroes and drops writes. The probe finds 16 MB, no
   page goes above it, and the game reaches the swamp. (The comment this
   replaced said the wrap *was* how the size is found; it is how the size is
   found wrongly.)

Neither fix changes Shrek 2's default run: fields 600, 1,200 and 3,700 are
byte-identical to before. The story pages advance with A ("Press A to skip")
and not on their own, which may be the narration never finishing - there is
no sound yet (P5).

### P7: platforms and export

As the playbook: `tools/consoles.sh`, `build_steamos.ps1`, the macOS
workflow, `create_distribution.py`, `test_release_bundle.py` and the release
workflow. The A/B determinism check uses the reference rasterizer. The
`ID.gcdata` file has to be carried by every export path, which is the one
place the new deviation touches shared code. Windows export already carries
it (the backend's `install_files()`); Linux and macOS do not yet, and the hub
refuses those targets for GameCube until they do. `tools/create_distribution.py`
has a GameCube row (2026-09-13) and `tests/test_release_bundle.py` a smoke
recipe for it (a generated DOL, run under `--differential`); the release
workflow's archives now carry `gcrecomp` and `runtimes/runtime-gc`.

### P8: accuracy and speed

Timing programs, then performance. A 485 MHz PowerPC is the first guest whose
CPU speed is a real constraint, and the renderer is likely to dominate long
before the CPU does - so measure before optimising, as the `NL*` work did.

## Checklist coverage

There is no supplied `gamecube_assembly_to_c_nuances.txt` for this console;
this table is derived from the Gekko manual, YAGCD and the `hwtests` suite.
Every row gets a test before the phase that owns it closes.

| Nuance | Test |
|---|---|
| Big-endian guest on a little-endian host: a structure written as words and read as bytes | vectors: mixed-width access to one address; `hwtests/cputest/load.cpp` |
| Unaligned loads and stores, including across a page or BAT boundary | vectors; alignment-exception cases |
| `lmw`, `stmw`, `lswi`, `lswx` register-file wraparound | vectors |
| `rlwinm`, `rlwimi`, `rlwnm` mask generation where MB > ME | vectors; `hwtests/cputest/rlw.cpp` |
| `srawi` and `sraw` carry-out into XER[CA] for negative values | vectors; `hwtests/cputest/srawix.cpp` |
| XER[SO] stickiness and the OE-form overflow rules | vectors |
| CR update forms; `mcrf`, `mcrxr`, `mfcr`, `mtcrf` field masks | vectors; `hwtests/cputest/cr.cpp` |
| `cntlzw`, `extsb`, `extsh`, division by zero, and `0x80000000 / -1` | vectors |
| `lwarx` and `stwcx.` reservation, cleared by a store to the reserved block | vectors; a reservation unit test |
| `frsp` rounding of a double to single, including the denormal range | `hwtests/cputest/frsp.cpp` |
| `fctiw` and `fctiwz` out-of-range and NaN results, and the undefined high word | `hwtests/cputest/fctiw.cpp`, `fctiwz.cpp` |
| FPSCR FPRF class bits after every FP operation | `hwtests/cputest/fprf.cpp` |
| NaN propagation: which operand survives, and where a QNaN is generated | `hwtests/cputest/nan.cpp` |
| Non-IEEE mode: denormal flush on input and on output | `hwtests/cputest/ni.cpp` |
| `fres` and `frsqrte` estimates, exact to the manual's tables | `hwtests/cputest/reciprocal.cpp`; vectors over the whole table |
| The `fmadd` family: one rounding of the product-sum, not two | vectors |
| Paired singles: PS0/PS1 lanes, `ps_merge`, `ps_sel`, `ps_sum`, the cross forms | `hwtests/cputest/pairedmove.cpp`; vectors |
| Quantised load/store: GQR scale exponent and type (u8, u16, s8, s16, float), with saturation | vectors over every (type, scale) pair |
| `stfs` of a double that was never converted, and `lfs` into both lanes | vectors |
| HID2 gating: paired-single and quantised instructions when PSE or LSQE is clear | vectors |
| Write-gather pipe: 32-byte gathering at `0xCC008000` into the CP FIFO, and the flush needed before a read-back | FIFO unit test; `gxtest` |
| Locked cache at `0xE0000000`, `dcbz_l` allocation, DMA through DMAU/DMAL | locked-cache unit test |
| `dcbz` zeroing a 32-byte line, including on uncached memory | cache unit test |
| `sync`, `isync` and `eieio` as the ordering points our bus model honours | code-in-RAM test: a program that writes code and jumps into it |
| Timebase and decrementer at bus/4; `mftb` monotonic across a slice yield | timing unit test; `--differential` |
| Exceptions: DSI, alignment, program, decrementer, external - and the `SRR0`/`SRR1` each leaves | exception unit test |
| `MSR[EE]`, `MSR[FP]`, `MSR[DR]`, `MSR[IR]` transitions and `rfi` | vectors and the exception test |
| Cached versus uncached aliases of the same memory (`0x80000000` versus `0xC0000000`) | bus unit test |
| GX vertex descriptors: direct, 8-bit indexed and 16-bit indexed attributes | `gxtest` |
| TEV combiners, konstants, clamping and the bias and scale ladder | `gxtest/tev.cpp` |
| Rasterization sample points and pixel coverage | `gxtest/rasterization.cpp` |
| Clipping against the guard band | `gxtest/clipping.cpp` |
| Lighting: attenuation forms, diffuse modes, channel control | `gxtest/lighting.cpp` |
| EFB copy filter, gamma, and intensity copies | `gxtest/copyfilter.cpp`, `intensity.cpp` |
| Texture formats, mipmapping, and the CMPR block layout | texture unit test with reference blocks |
| ARAM DMA alignment and the 16 MB wrap | DSP unit test |
| AX microcode: voice mixing, ADPCM, sample-rate conversion | audio unit test against a recorded mix |
| DVD read timing and the completion interrupt | DI unit test |
| Memory card block allocation and the CARD directory checksum | card unit test |
| SI polling cadence and controller origin calibration | SI unit test |

## Status

| Phase | State |
|---|---|
| P0 | **Done 2026-09-11.** Licenses checked, reuse decided, plan written, `tools/fetch_gc_tests.py` and `roms/gc/MANIFEST.json` in place, fetch gate passed. |
| P1 | **Gate met 2026-09-11**, except the UBSan build and `hwtests/cputest` (see below). The decode classifier is generated from the ISA table (222 opcodes) and checked over all 131,072 dispatch positions. The interpreter implements every one of them. **2,490 vectors measured on the oracle pass, 0 failures**, in Release, Debug and AddressSanitizer builds; 66 cases are listed deviations. CTest: `gc_decode`, `gc_cpu_units`, `gc_vectors`, `gc_decode_header_current`, `gc_vector_cases_current` - 5/5. Not done: a UBSan build (no clang or gcc on this machine; the Linux CI job in P7 covers it), and running `hwtests/cputest`, which needs the machine model and has moved to P2. |
| P2 | **Gate met 2026-09-12.** The machine boots the SDK and runs both suites. `hwtests`: **10 of the 13 `cputest` programs pass outright**, one (`fctiw`) sweeps 771 million cases and is a matter of hours rather than a failure, and two (`pairedmove`, `reciprocal`) have a residue in the same denormal corner of the paired estimates - 388 of 8,800 subtests and 1 of 17,411. Ours: **`gc_memtest` and `gc_timetest` pass outright**, 636 and 15 subtests, after fixing the five things they found (see [Our own test programs](#our-own-test-programs)). Everything else in P2 is built: BAT translation, the physical map, the I/O register file, level-triggered interrupts, the DSP boot handshake, the loaders and `gc_run`. `tools/gc/run_gc_tests.py` holds every program to `tests/gc/hwtest_baseline.txt`, and CTest runs it whenever they are built (7/7 GameCube tests pass). The DVD interface is modelled and `gc_disc.c` reads images: **`gc_disctest` passes outright**, 29 subtests, with libogc's own DVD driver mounting the disc and reading files off it. **The P2 gate is met.** |
| P3 | **Started 2026-09-12.** The command stream is read: the FIFO ring, display lists, the CP, XF and BP register files, the vertex descriptor and attribute tables, and the two pixel-engine interrupts. All seven `gxtest` programs run to completion with **zero commands the reader could not parse** - 1.8 million bytes and 143,489 commands for `lighting` alone - and `gxtest_bitfield` passes outright. The transform unit and primitive assembly follow, and then the framebuffer and the copy out of it - which takes **`gxtest_intensity` to passing, 262,144 assertions a block**, without a rasteriser, because that program fills the framebuffer by poking it. Then the rasteriser, which takes **`gxtest_rasterization` to passing - all 519 subtests, including the sub-pixel sample point** - and `gxtest_clipping` from 10 failures to 7. Then the texture environment - **`gxtest_lighting` passes, 65,536 subtests**, and `gxtest_copyfilter` passes eight blocks - and the clipper, which takes `gxtest_clipping` to 4 failures of 16, all four being steps that read a pixel their own geometry does not cover. **Five of the seven `gxtest` programs now pass.** The copy to the display buffer and its YUV conversion follow, with `gc_run --dump-frame` and `gc_drawtest` to exercise them: **a frame comes out end to end and is correct**. Then the texture unit: five formats pass their own test, since no hardware test samples a texture at all. Still to come: filtering and mipmaps, texture coordinate generation, the lights themselves, blending and alpha compare, and the video interface's own timing. |
| P4 | Not started. |
| P5 | **Gate met 2026-09-13.** Saves, the audio unit tests and the overlay menu earlier; the 30-second windowed run of Shrek 2 with no underruns (1,798 fields, 59.9 a second) once the pixel path went to helper threads. A 90-second run is still 12 % short. See [Where P5 stands](#where-p5-stands). |
| P6 | **Gate met 2026-09-13** with Shrek 2: imports, builds (36 s; 41 s with the cover's trace), plays into gameplay, gets a cover, keeps its memory card across a rebuild, is **clean under `--differential` for 6,300 scripted fields**, and reports its native share (96.951 % by instructions and 40.376 % by address without a trace; 99.995 % by instructions with one). Windows export works; a release packs `gcrecomp` and `runtimes/runtime-gc`. See [P6](#p6-hub-integration-and-the-first-game). |
| P7 | Not started. |
| P8 | Not started. |

## The vector pipeline

PowerPC has no SingleStepTests suite, so the vectors are made rather than
downloaded. Nothing this project ships depends on the tools below: the
vectors are checked in, and CI replays them.

```text
tests/gc/vectors/gen/       a GameCube program: load a register state,
                            execute one instruction, print both states
  tools/gc/gen_vector_cases.py   the case table, generated from the same
                                 isa.yaml the decoder comes from
  tools/gc/build_vector_gen.py   devkitPPC builds it into an ELF
  tools/gc/capture_vectors.py    Dolphin runs it; the log becomes
                                 tests/gc/vectors/gekko.vectors
  tests/gc/gc_vectors.c          replays that against the interpreter
```

Installed on 2026-09-11, both without administrator rights:

| Tool | Version | Where | Notes |
|---|---|---|---|
| devkitPPC | r50 (gcc 16.1.0) | `D:/devkitPro` | Installed from the pinned devkitPro pacman packages rather than the graphical installer. Set `DEVKITPRO` and `DEVKITPPC`. |
| Dolphin | 2606a | `D:/Tools/Dolphin/Dolphin-x64` | Run as a tool, never linked or copied. |
| QEMU `-cpu 750` | not installed | - | A second oracle for the scalar subset. Optional, and it is what would turn "reproducible" into "corroborated". |

Three things have to be true before the oracle produces anything usable, and
`capture_vectors.py` sets all three in a Dolphin user directory of its own so
that it never touches a real Dolphin installation:

- **The interpreter CPU core, not the JIT.** A JIT is free to leave anything
  it likes in the half of a register the instruction does not define - its
  SSE double-precision arithmetic leaves frA's ps1 in the destination's ps1 -
  and those are artefacts of a recompiler, not of a CPU.
- **`DebugModeEnabled`**, because the `OSReport` hook is flagged debug-only.
- **The `OSREPORT_HLE` log channel**, which is where that hook writes. The
  `OSREPORT` channel is the EXI one and stays empty.

Guest output goes out as `OSReport("%s\n", text)`: Dolphin decides which
register holds the format string by looking at what it points at, and a bare
string can be mistaken for a `this` pointer.

**Floating point must be compiled strictly.** The FPU model depends on each
operation rounding exactly once and on the host's exception flags being
readable straight afterwards. MSVC's default `/fp:precise` contracts
`a*b+c` into a fused multiply-add and moves operations across flag reads; it
cost 46 vector cases before `runtimes/runtime-gc/CMakeLists.txt` set `/fp:strict`
(and `-ffp-contract=off -frounding-math` elsewhere). Every generated project
will need the same setting.

## What the vectors settled

The run measured 2,490 cases over 415 encodings. These are the behaviours it
pinned that were wrong, missing or guessed beforehand - the reason the phase
exists:

| Behaviour | What it turned out to be |
|---|---|
| Single-precision results and ps1 | With `HID2[PSE]` set, every single-precision instruction - `fadds`, `fsubs`, `fmuls`, `fdivs`, the `fmadds` family, `fres` and `frsp` - writes its result into **both** lanes. The manual says so; missing it leaves stale data in half of every vector a game computes. |
| The class a single-precision result reports | `FPSCR[FPRF]` is the class of the **single**, not of the double it is stored in. A result that is denormal as a single is an ordinary double, and calling it normal hides an underflow from the guest. |
| The multiplier is 25 bits | A single-precision multiply rounds frC to 25 significand bits first, so `fmuls` and the paired multiplies can differ from a plain double multiply by an ulp. |
| Single-precision fused multiply-add | Rounding the double FMA result a second time lands on the wrong side of a tie when the double is exactly halfway between two singles. The exact residue has to break the tie. |
| Non-IEEE mode and single denormals | `FPSCR[NI]` flushes results that are denormal **as singles**; the double-denormal rule alone is not enough, because 2^-149 is a perfectly good double. |
| `stfs` and `psq_st` of a float | The double-to-single conversion is defined bit by bit and **truncates**: 0.1 goes out as `3dcccccc`, not `3dcccccd`. Values below the single normal range are denormalized by shifting rather than flushed, which a host cast gets wrong in both directions. |
| `fctiw`, `fctiwz` | The undefined high word is `0xfff80000`, except that a zero result from a negative operand carries the sign into bit 32. |
| `ps_sum0`, `ps_sum1` | The lane that is only passed through still goes through the single-precision rounding stage. |
| XER | Only `0xe000ff7f` is writable: SO, OV, CA, bits 16-23 and the byte count. Bits 3-15 and bit 24 read back as zero. |
| FPSCR | Bit 20 is reserved and reads back as zero; FEX and VX are derived from the other bits and cannot be written. |
| `mtfsb1` | Setting a bit by hand does not set the FX summary. |

## What the oracle does not model

Dolphin's interpreter leaves some bits alone that the architecture defines,
so the comparison masks them rather than copying the gap. Everything else -
every register value, every lane, the scratch memory, CR, XER and the rest of
FPSCR - is compared exactly. `hwtests/cputest` is what settles these on
hardware, in P2.

| Not compared | Why |
|---|---|
| `FPSCR[FI]`, `FPSCR[FR]` | Never set for ordinary arithmetic by the oracle. |
| `FPSCR[XX]`, `[OX]`, `[UX]`, and `[FX]` with them | A single-precision add whose result leaves the single range raises none of them there, though the architecture says it must. FX is their summary, and CR1 carries the same two bits on an Rc form. |
| `FPSCR[FPRF]` on the compare instructions | The oracle updates the CR field but not FPCC. |

## Running hwtests

The suite is the hardware layer for P2 and P3, and getting it to run took
three discoveries worth writing down.

1. **It is built for a Wii, not a GameCube.** Its CMake passes `-mrvl`,
   links against `libogc/lib/wii` and pulls in `wiiuse` and `bte`. The
   `cputest` programs are pure CPU and would run either way, but the
   start-up around them is the Wii's. Building them for a GameCube means
   overriding `MACHDEP` and the library directory, which
   `tools/gc/build_hwtests.py` will do by building from a patched copy
   rather than by editing the fetched tree, so that `roms/gc` keeps matching
   its manifest.
2. **`iostest` does not build at all** against a current libogc, and does
   not need to: it tests the Wii's IOS, which a GameCube does not have. The
   builder takes `--modules`, and leaves it out by default.
3. **fmt is a submodule**, and a source tarball carries no submodules, so
   the fetch script pins it as its own suite into the directory hwtests
   expects.

All 13 `cputest` programs build and run. `tools/gc/run_gc_tests.py` holds
them, and our own programs, to `tests/gc/hwtest_baseline.txt`.

## What the hardware tests found

The vectors are a second implementation; `hwtests` is hardware. Where they
disagreed, hardware won, and every one of these was something this
implementation had wrong:

| Behaviour | What it turned out to be |
|---|---|
| Non-IEEE mode and operands | It does not flush them, only results - whatever the manual's Table 2-21 says. |
| Tininess | Judged before rounding. x86 judges it after, so a value a hair under the smallest normal that rounds up to it raises nothing; recomputing toward zero settles which side the exact result fell on. |
| Which NaN survives | Position, not kind: a quiet frA beats a signalling frB. |
| Widening a single | Must not quiet a signalling NaN. A C conversion does; `psq_l` is a conversion, not an operation. |
| `frsp` of a NaN | Drops the mantissa bits a single does not have. |
| A compare and FPRF | Writes only the four condition bits and leaves the class bit above them alone. |
| The paired-single lanes | The datapath cannot carry a double: **what lands in ps0 is rounded to a single, what lands in ps1 is truncated**. It shows on `ps_mr`, the merges, `ps_neg`/`ps_abs`/`ps_nabs` and the lane `ps_sum0`/`ps_sum1` pass through. The oracle does not model this, so those instructions are listed as oracle deviations in the vector harness. |
| `fres` out of range | Saturates to the largest single rather than overflowing, and becomes zero past the bottom. |

## Our own test programs

`hwtests` is pure CPU: it runs out of registers and says nothing about the
machine underneath. `tests/gc/programs/` is the other half - ours, MIT, in
this repository - built by `tools/gc/build_gc_programs.py` and reported
through the same verdict lines, so one runner and one baseline cover both.

`gc_memtest` covers the boot state, byte order, the byte-reversed forms,
unaligned accesses across a cache line and across a page, the cached and
uncached views of one memory, `dcbz`, the multiple and string forms,
quantised load and store, the locked cache and its DMA, the write-gather
pipe, and reservations. `gc_timetest` covers the timebase, the decrementer
and its interrupt, a libogc alarm, the video field rate and the beam
position, and sleeping.

Where the answer is a convention rather than an arithmetic fact, the
programs ask libogc to do it rather than encoding a reading of the manual:
`LCLoadData` programs the locked-cache DMA registers, `CAST_SetGQR2` builds
a GQR, `VIDEO_WaitVSync` waits on a retrace. libogc ran on the hardware for
twenty years, so a disagreement with it is ours.

What they found, all of it now fixed:

| Behaviour | What it turned out to be |
|---|---|
| The video field rate | 60 Hz, rounded. NTSC is 60000/1001. Off by one part in a thousand, which is a second of audio drift every quarter of an hour; it is now derived from 27 MHz, 1716 VI clocks a line and 262.5 lines a field, which comes to exactly 8,108,100 core cycles. |
| The beam position register | The two halves were the wrong way round, and the horizontal one was the constant 1. `VIDEO_GetCurrentLine` reads the vertical half twice around the horizontal read and retries if it moved, so it returned a constant forever. |
| The locked-cache DMA | The addresses arrive with their top bits already meaningless: libogc writes the cache address with `0xe0000000` masked off and the memory address with `0x80000000` left on, and both work on a console. The memory side is a physical address with the view-selector bits dropped; the cache side is an offset into the 16 KB. The register layout itself - trigger, direction, and seven bits of block count split across the two registers - was right. |
| The write-gather pipe | Gathered bytes were counted and dropped. They go to the physical address in WPAR, 32 bytes at a time, and only while `HID2[WPE]` is set - which libogc leaves clear until `GX_Init`. |
| Reservations | `lwarx` recorded an effective address and the store that should clear it compared a physical one, so a reservation was never broken by an intervening store. It is a physical block, and the bus keeps it. |

## The command stream

Flipper is not driven by registers. A game writes a *byte stream* through
the write-gather pipe into a ring buffer in main memory, and the graphics
processor reads that ring and executes it: a register write, a matrix, a
display list and a triangle are all commands in the same stream.

`runtimes/runtime-gc/src/gc_gx.c` is the reader. The FIFO plumbing is modelled the
way the hardware wires it - the pipe's 32-byte bursts land at the processor
interface's write pointer, wrap at the end of the ring, and the command
processor reads from its own pointer behind them - so display lists, a
partly delivered draw and `GX_Flush`'s eight words of padding all work
because they work on a console, not because anything here special-cases
them.

The piece that has to be exact is **how big a vertex is**. A draw command
carries a count, not a length, and the length comes out of the vertex
descriptor (two CP registers) and the attribute table (three more, per
format, with the texture coordinates spread across all three and the fourth
one's fraction in a different register from its format). A mistake there
does not draw a wrong triangle: it loses the reader's place, and every
command after it is noise. So `gc_gx_vertex_size` is public, is tested on
its own in `tests/gc/gc_gx_test.c`, and the runner reports commands it could
not parse rather than skipping them.

That last number is the measurement worth having, and it is zero across the
whole suite: `gxtest_lighting` alone pushes **1,829,792 bytes and 143,489
commands** through the reader with nothing unaccounted for.

The two pixel-engine interrupts are here too, because they come out of the
stream rather than from the CPU: a game writes a token or a "done" and waits
for the interrupt that arrives when the graphics processor reaches it.
Without them `GX_DrawDone` waits for ever, which is a hang and not a wrong
picture.

Field layouts come from devkitPro/libogc's GX (zlib, on the reuse list) and
from YAGCD. Dolphin's are read as a behaviour reference and not copied.

## The rasteriser

Two things about it are not the obvious choice, and both were measured on a
console by whoever wrote `gxtest/rasterization.cpp`.

**A pixel is not sampled at its centre.** It is sampled just above seven
twelfths of the way across. The test pins that down by sweeping a viewport
four hundred-thousandths of a pixel wide across the sample point one float
at a time, 515 times, and asserting a clean run of 513 hits with one miss on
each side - which brackets the sample to a window about one
hundred-thousandth of a pixel wide. A rasteriser that snaps coverage to a
sub-pixel grid cannot produce that answer at all.

**Screen coordinates carry an offset of 342** in both axes. The viewport
registers have it added in, the scissor box has it added in, and the scissor
*offset* register is what takes it out. So the transform leaves it in and
the rasteriser subtracts twice the scissor offset - which is what the
hardware does, and what lets a game render in strips by moving one register
instead of rewriting every viewport.

One consequence that had to be found by failing: **the viewport transform
cannot be done in single precision.** Near 342 a float's steps are three
hundred-thousandths of a pixel, which is most of the width of the sliver the
test draws, so rounding the multiply-add alone puts it on the wrong side of
the sample point. Doing that one step wider reproduces every measured point.
The projection above it stays in single precision, because that is what the
transform unit computes in.

**`gxtest_rasterization` passes**, all 519 subtests. `gxtest_clipping` is
down to 7 of 16: the rest need a real clipper, and two of them need the
hardware's clip arithmetic, which is not IEEE - at one input the exact sum
is -1.19e-7 and the hardware treats it as zero and draws the quad.

## The lights, and the coordinate generators

Both are in `docs/gc-lighting-texgen.md`, and both are mostly a matter of
not believing the obvious reading.

**A light's attenuation is a ratio of two quadratics in two different
variables.** For a spot light the numerator is a quadratic in the cosine of
the angle off the cone's axis and the denominator a quadratic in the
distance; for a specular one both are in the same cosine. Feeding the
cosine into both is the usual way a spotlight comes out looking like a
specular highlight. The numerator is clamped to zero and the denominator is
not, so a negative attenuation is reachable.

The stored direction words already point *at* the light: libogc negates on
the way in. The ambient term is the accumulator's starting value rather
than a stage of its own, which is why a lit channel with no light in its
mask is material times ambient and not black - the one part of this the
hardware test actually measures, across all 65,536 pairs.

**Three things here are choices, not hardware behaviour**, and the comments
say so: the order lights accumulate in, whether the accumulator saturates
per light or once at the end, and what a zero denominator does. Zero is
returned for the last of those, to keep a light from becoming infinitely
bright at a single point.

**The coordinate generators' enumerations are not what the registers
hold.** Two of the mappings are traps. The four three-component sources are
rows 0, 1, 3 and 4 with row 2 reserved for colour, so the API's order and
the hardware's differ and a table is needed rather than an offset. And the
two matrix types are inverted relative to the projection bit - the
three-row type is enum 0 and *sets* the bit, the two-row type is enum 1 and
clears it - so transcribing the enum straight into the register gets every
matrix generator backwards.

Only the three-row type divides, and its third row exists for no other
reason.

**Unverified, and it matters:** whether the position and normal sources
deliver the untransformed attribute or the value after the matrix. Neither
source says, and it changes the output of every environment map. This
implementation passes the transformed value, which is what the rest of the
pipeline has to hand; a hardware test would settle it.

## The output stage, and a dither that should not fire

Between the texture environment producing a colour and that colour reaching
the framebuffer: the alpha test, the depth test, blending, and two separate
enables for whether the colour and the alpha are written at all.

Three things worth recording.

**The blend factors are not symmetric.** The same numeric value means "the
destination's colour" when it is the source's factor and "the source's
colour" when it is the destination's - libogc gives the two names and the
same number.

**Where the depth test happens is a register bit.** With early depth
testing on, a pixel the alpha test later kills has already written its
depth; with it off, it has not. Games rely on both, so the rasteriser does
the test in whichever place the pixel format register asks for.

**Dithering only does something when there is something to dither.** It
exists to hide the banding a narrow framebuffer causes; with eight bits a
channel stored there is nothing to hide, and perturbing the value is
visible error rather than dither. libogc turns dithering on in `GX_Init`
and most programs never turn it off, so getting this wrong is not a corner
case: applying it unconditionally moved every colour by a step or two and
took `gxtest_clipping` from 4 failures to 10, `gc_textest` from five
passing formats to none, and `gc_drawtest` from both blocks to one. It now
fires only for the two formats narrower than eight bits, which is the
physical rule. The matrix itself is still **unverified** - nothing
available here measures it.

**2026-10-07: the blend's scale and the dither changed.** Two rules here
had no evidence behind them and together made any scene built from many
blended layers too dark:

- the blend multiplied by factors of 0 to 255, so "one" was 255/256 and
  `(s * f + d * 255) >> 8` cost the destination a level for every layer,
  whether or not the layer added anything;
- the dither was a four-by-four matrix with an offset of -2 to +1, applied
  after the blend in both narrow formats. In the six-bit format the
  rounding that follows takes four levels off wherever the offset is
  negative - half the pixels, again for every layer.

Shrek 2 draws the light shafts of its swamp as about 187,000 large
triangles a cutscene, blended source-alpha plus one into a six-bit
framebuffer with dither on, so both applied several times over to most of
the screen. Found by the fast OpenGL path, which was a mean of five levels
brighter than this rasteriser and turned out to be the one closer to
right.

Now a factor gains its top bit, 0 to 256, so "one" is one - the rule the
combiner already uses for its own interpolation weight (`c + (c >> 7)`) -
and the dither is a two-by-two matrix of 0 to 3 added after the value's
own top two bits are taken off, in the six-bit format only. That
subtraction is what makes it harmless: a stored six-bit value passes
through unchanged, so a layer that changes nothing changes nothing. Both
are what Dolphin's software renderer does (GPL; read as a behaviour
reference, nothing copied). **Neither is measured against hardware**:
hwtests never blends, and its one six-bit case is switched off in its own
source as not working. If a measurement turns up, it wins.

The same dwarf cutscene, eighteen frames, before and after: 86 % of pixels
differ, 34 % by more than 8 levels, mean 5.9. `gc_gx_test` holds the new
rules (one plus one is the sum; a transparent pixel leaves the destination
alone; fifty transparent layers leave a six-bit pixel alone at every place
in the matrix; five-six-five is not dithered), `gc_blendtest`'s expected
values follow, and the exact OpenGL path's shader is the same arithmetic:
`compare_gl.py` agrees on 11 programs in three shader modes, and
`gc_gl_fuzz` on 1,500 random pipelines shows no colour or depth
difference. The hardware tests' baseline is unchanged (28 programs).
State hashes and frames of any game that blends are different from
before this date.

## Textures, and the one part with no hardware evidence

`hwtests` never samples a texture. Its `tev` program sets every stage's
texture-enable bit to zero, and no other program binds one - so the texture
unit is the largest part of the pipeline with no measured evidence behind
it at all.

What it has instead is `docs/gc-texture-formats.md`. Its tile geometry and
address arithmetic come from libogc, which is authoritative for those; but
libogc never touches a texel byte, so the per-texel decode rules came from
somewhere else. Four permissively licensed decoders - noclip.website,
aurora, gclib and GRRLIB, all MIT - settled nine of thirteen open
questions, and one of them was **wrong in the document**: alpha is the high
byte of an IA8 texel, not the low one. Four sources agree, including an
*encoder* whose output goes to real hardware on every draw. Three rules
remain open and one is disputed, and the document says which.

`tests/gc/programs/gc_textest.c` is what stands in for a hardware test: it
builds an image of each format byte by byte in the tiled layout, draws it
one texel to one pixel through libogc's own GX, and reads the framebuffer
back. Five formats pass - 565, eight-bits-a-channel, intensity, intensity
with alpha, and paletted - which checks the tiling, the channel widening,
the two-plane layout, and the corrected IA8 rule.

A palette is not addressed in place: software points one register at it in
main memory and another at where in the texture cache it should go, and
writing the second performs the copy. That memory is the upper half of the
cache, 512 KB, named in 512-byte units - so 1024 units of 256 entries.
Sizing it by guess rather than by that arithmetic leaves every load falling
off the end and every paletted texture black, which is exactly what
happened first.

Still missing: bilinear filtering, mipmaps, the texture cache itself, the
compressed format's own test, and texture coordinate generation beyond
passing the vertex's coordinates through.

## Getting a frame out

A copy to the display buffer is not a copy to a texture with a different
destination. Bit 14 of the trigger overrides the format field entirely: the
result is always YUV 4:2:2, two horizontally adjacent pixels sharing one
pair of chroma samples, packed as luma, blue-difference, luma,
red-difference - which read as a big-endian word is `0xY0CbY1Cr`. It is
linear, two bytes a pixel, with a pitch the stride register gives in units
of sixteen pixels.

The conversion is studio-swing BT.601, so black is luma 16 and white is
235. That is not a guess: libogc ships a table of pre-converted fill values
for twenty named colours, and eighteen of them come back byte-exact from
these coefficients.

The two pixels' chroma are **not** averaged. The chroma sample sits on the
macropixel's even pixel and is a three-tap filter reaching one pixel either
side of it, `(c[2k-1] + 2*c[2k] + c[2k+1]) / 4` with the remainder thrown
away and the left tap repeating at the edge of the rectangle. The table of
constants cannot tell those rules apart - every colour in it is solid, and
a solid colour filters to itself - so this was settled at a seam instead,
by fitting a reference renderer's colours at the first macropixel of each
of four solid bands back to chroma. The answer is exactly one part in four
of the band before, three boundaries agreeing: not the half that averaging
across the seam gives and not the zero that averaging within the pair
gives. Evidence layer: differential against a second implementation, not
hardware. See docs/gc-video-out.md.

`gc_run --dump-frame FILE.ppm` writes the frame the video interface is
scanning out, decoded back to red, green and blue; `--dump-frame-every N`
adds a numbered one every N fields. The buffer, its stride and its size come
from the video interface's own registers (`gc_vi_display`), because that is
what a television shows however the pixels got there. The last display copy
is the fallback, for a program that never points the video interface at a
buffer.

`tests/gc/programs/gc_drawtest.c` is the program that exercises it: it sets
the machine up the way the devkitPro examples do, draws five flat quads
through libogc's GX, and checks both ends - the framebuffer through a peek
and the display buffer through the luma it was converted into. None of the
`gxtest` programs ever copies a frame out for display, so without this the
path a real game uses sixty times a second would have no test at all.

Two things it found immediately. The clear colour's registers pair alpha
with red and green with blue, each in the high and low byte of its own
register, and this had them wrong. And a game's first clearing copy is what
puts the far value into the depth buffer: without it the very first depth
test rejects every pixel, and the screen stays black with no other symptom.

### Comparing a frame against a reference

`tools/gc/capture_reference.py` runs an ELF under Dolphin's software
renderer and writes the dumped frame as a PPM; `tools/gc/compare_frame.py`
holds ours to it. `gc_drawtest` ends by handing its buffer to the video
interface and waiting out four fields, because a reference emulator only
dumps frames that are actually *presented* and without that there was
nothing to dump.

Closing that loop found the chroma filter above, and it found something
about the reference. Two settings matter and neither is a default:

- `[Enhancements] DisableCopyFilter = False`. Dolphin ships with the
  vertical copy filter **off**, because the deflicker weights games ask for
  make the picture blurry and most people would rather it did not. libogc
  hands an interlaced mode's `{8,8,10,12,10,8,8}` to `GX_SetCopyFilter`,
  which groups two-three-two into 16, 32, 16 and softens every horizontal
  edge across two rows. With the reference's default left alone, the
  comparison flagged six rows - every horizontal edge in the frame - and
  what it was flagging was **our** accuracy, which `gxtest_copyfilter`
  verifies against hardware. The lesson generalises: when a second
  implementation disagrees, check which of the two has the stronger
  evidence behind it before changing anything.
- `FrameDumpsResolutionType = 2`, so the dump is the guest's own buffer at
  the guest's own size rather than a scaled one.

With both set, `gc_drawtest`'s frame matches: **zero pixels differ by more
than two, and the largest single-channel difference is two**, which is what
a round trip through YCbCr and back costs. That is the frame half of the P3
gate.

## Fog

The hardware does not hold the four distances a game passes to
`GX_SetFog`. It holds A, a 24-bit B magnitude, a shift, C and a three-bit
function select, and evaluates one fixed rational expression in the
interpolated screen depth: `A/(B - zs) - C` with a perspective projection,
`A*zs - C` without. The SDK performs the change of variables, and A and C
reach the registers as the **top twenty bits of their float words** with
the bottom twelve mantissa bits thrown away rather than rounded - so an
implementation that keeps the caller's original float drifts away from one
that rebuilds it from the twenty bits. `runtimes/runtime-gc/src/gc_gx_fog.c` rebuilds
it. The algebra and its derivation are in docs/gc-fog-indirect.md part A.

Fog sits after the last combiner stage and before the framebuffer blend,
and it never touches alpha: register `0xF2` has red, green and blue and no
alpha field at all. That is why its position relative to the alpha test
does not matter - the alpha test reads the combiner's alpha either way.

**Two parts of it are not established by anything read so far**, and both
are marked where they are implemented. The four exponential curves are
guesses: the SDK's header is a bare list of enumerators with no prose and
its source passes the three-bit select straight through, so the base, the
constant and the position of the square are all convention rather than
evidence. And the range adjustment's table indexing - how a distance from
the centre becomes an index and a weight, what happens inside the first 32
pixels, what happens past the tenth entry - is inference from the table's
shape. Neither is likely to matter for a picture that looks right; both
would matter for an exact comparison.

`tests/gc/programs/gc_fogtest.c` covers it, because no `gxtest` program
touches fog at all. Five white strips at five depths, fogged towards black,
give five points on the curve; the expectations for the linear case are
worked out by hand from the definition rather than from the code. It does
the orthographic path and then the perspective one, which is the half that
matters - orthographic fog is a multiply and a subtract, while perspective
fog is where the reciprocal, the B magnitude and the shift all have to be
right at once. The exponential block pins our assumed curve so it cannot
change unnoticed, and says in as many words that this is not the same as
knowing it.

## Mipmaps, and where the level comes from

There is **one address register a texture unit** and it points at level
zero. Every other level is derived, because the chain is contiguous - that
is the whole of the addressing scheme. A level occupies
`ceil(w/tile_w) * ceil(h/tile_h) * bytes_per_tile`, each axis halving
independently and flooring at one, and the ceiling is where garbage comes
from: a 2x2 level still costs a whole 32-byte tile, and a chain built
without that rounding has every level past it reading from the middle of
the one before. `tests/gc/programs/gc_textest.c` reaches the tail of a
chain deliberately for that reason.

The level itself comes from the gradient - how fast the texture coordinate
changes from one pixel to the next. The hardware gets it from the 2x2 quad
it rasterises at once; `gc_gx_raster.c` evaluates the same interpolation
one pixel right and one pixel down, which is the same quantity more
slowly, and takes the neighbours whether or not they are covered because
the interpolation is defined across the whole plane and a gradient taken
only from covered pixels would step at every silhouette.

The three filter bits are two independent things: bit 7 says how the four
texels *within* a level combine, bits 6:5 say how many levels are read.
The magnifying filter is a separate bit and never has a mip mode, which is
structural - a magnifying pixel reads one level, and that level is the
minimum of the range, so there is no such thing as magnifying level three.
The LOD range is unsigned, which means **a minimum above zero makes
magnification impossible**.

Two open questions are marked in the code. The log2 is the bit-trick
approximation - exponent for the integer part, top mantissa bits used
directly as the fraction - which sags by up to 0.086 of a level between
octave boundaries. The reference emulator's own two implementations
disagree about this, so it is not settled there either. And the tie at
exactly LOD 0 magnifies here, which is what the reference does and the
opposite of what the SDK's prose says. docs/gc-mipmap-lod.md has both,
with the tests that would settle them.

## The video interface's own timing

Two faults lived in the beam position register for as long as it existed,
and both were invisible to the test that produced it, because
`VIDEO_GetCurrentLine` takes the half-line index modulo one field's worth
and hides almost any error.

The horizontal half counts **pixels**, two VI clocks each, 858 to a line.
The vertical half counts across a whole **frame** - both fields, 0..524
for NTSC - and does not restart each field. The second one is what tells
the fields apart: the SDK decides which field is next by asking whether
the position is in the first half of the frame or the second, so a counter
that restarts every field always says "first" and `VIDEO_GetNextField`
never alternates. A game that renders one field at a time draws every
frame into the same half of the screen, and nothing else complains.

**Display interrupts are position matches, not field events.** Each of the
four registers names a line and a column, both counting from one, and
fires when the beam reaches them. Raising every armed one at the field
boundary gets the retrace rate right by a wrong route - the SDK aims one
at the top of the frame and one at the top of the second field and uses
the pair - while putting every raster split about 240 lines early. The
arithmetic that confirms the units: the SDK's first interrupt targets line
263 column 430, which is pixel 225,225 of 450,450, exactly half the frame.

A register whose line or column is still zero never matches, because a
counter starting at one never reaches zero. An interrupt enabled but not
yet aimed produces nothing, which is the hardware's answer too.

There is **no vertical scaler and no anti-flicker filter in the
interface** - all vertical resampling is in the EFB copy. docs/gc-vi-timing.md
has the per-mode register tables and what is still unverified.

## The controllers

Four ports on the serial interface, each answering a short exchange of
bytes through a 128-byte buffer the four channels share. A command goes
out, a reply comes back, and the control register at `0x34` says which
channel, how many bytes each way, and when it is done. The layout did not
have to be looked up: a trace of a real game asking shows `c0010301` -
channel 0, one byte out, three bytes in, start - which is the "what are
you" command, and its answer is `0x09000000`, a standard controller.

`gc_pad.h` is what a frontend fills in; nothing in the runtime knows about
a keyboard or a gamepad. The sticks are signed and centred on zero there,
because that is what a caller finds natural, and are biased by 128 on the
way out because that is what the hardware reports.

**That bias is the whole risk in this piece.** A controller at rest reads
128 on both axes, not 0. Get it backwards and a game starts with the stick
held hard into a corner and reads it as the player doing that - which
looks like a game that will not stop walking left, not like an
input-decoding fault. The same goes for the origin: a game reads where the
sticks rest once, subtracts it from everything afterwards, and an origin
that disagrees with the resting position puts every later reading off
centre by a constant.

An empty port answers too, and what it answers is "there is nothing here".
A port that says nothing at all leaves a game waiting - a quieter failure
and a harder one to see.

`tests/gc/programs/gc_padtest.c` runs twice against the same machine: once
with a controller at rest, where the question is whether the sticks really
are centred, and once - `gc_run --pad-test` - against a deliberately
awkward pattern, a negative stick, an off-centre second stick and two
different trigger positions, that no plausible sign or bias mistake would
reproduce. The program is not told which it is looking at; it reads the
controller and the answer has to be one or the other.

One thing the test had to be talked out of asserting: the SDK's header
names the channels from the top (channel 0 is `0x80000000`) but the
function that reports which ports are connected numbers them from the
bottom, and nothing says so. Asserting the bit would have been asserting a
reading of undocumented behaviour, so it asserts the count instead - one
port answers and three do not - which is what the test is about.

## Booting a game, and the six things it needed

The first commercial disc arrived on 2026-09-12: Shrek 2, as a CISO.
`gc_run DISC` boots it and `gcrecomp DISC` recompiles its DOL. It runs
clean - no accesses to addresses that are not memory, no registers nobody
has described - and gets as far as calling the SDK's halt, which means it
**panicked** rather than hung. That is further than it sounds and not yet
a game.

Six things it needed that no test program ever asked for. Each was found
by measurement after at least one wrong guess, and each is a case where
the wrong behaviour looked like something else entirely.

1. **A DOL's BSS overlaps its own data sections, and the order of loading
   decides whether that matters.** The descriptor is one address and one
   length, and a linker uses it for `.bss`, `.sbss` and `.sbss2` - which
   are not contiguous. The small *data* sections sit between them and are
   real loaded sections inside the range it covers. Zero first, then load.
   The other way round erases initialised small data, which is exactly
   where a compiler puts the globals reached most often, and the game
   faults on a null pointer four hundred thousand instructions later.
2. **A system call returns to the instruction *after* `sc`.** Every other
   exception returns to the one that caused it, because it has not
   completed; `sc` has. Returning to the `sc` executes it again for ever.
   No test program uses `sc` and a commercial game uses it on every cache
   flush, so this cost nothing for months and then cost everything.
3. **The boot-info block.** BI2 is at disc offset 0x440, 0x2000 bytes, and
   the apploader leaves a pointer at 0x800000F4 that games dereference
   without checking.
4. **The console's own settings.** Sixty-four bytes on EXI channel 0,
   command 0x20000100: language, video mode, screen offset, clock bias,
   and a checksum that is a sum and a sum of complements over everything
   after the two halfwords holding it. Zeroes do not check out.
5. **Audio memory wraps.** Sixteen megabytes decoded by the low
   twenty-four bits of the address - and that is the mechanism the SDK
   uses to find out how much there is, by writing past the end and reading
   address zero. Refusing the transfer makes the question unanswerable.
6. **The bottom of the arena is the end of the loaded program**, not zero.
   The apploader computes it and the allocator reads it. Left at zero, the
   game built a heap at 0x7E000000 and wrote to it.

**Unverified:** bit 0 of the ARAM mode register at 0xCC005016 reads as
set. The game polls it, nothing writes it, and setting it let the game
continue - which is the only evidence there is for it.

### Then: let the disc load its own game

The six above got the game to the point of panicking inside its own
start-up. What fixed that was to stop loading the game ourselves.

Every disc carries a small PowerPC program at offset 0x2440 whose job is
to read the executable and put it where it belongs. Loading the DOL
directly instead is quicker and nearly works - what it misses is where
free memory begins. The word at 0x80000030 says so, and the SDK's
start-up clears everything from there to the top of the arena. A game's
stack sits immediately above its own image, and there is no reading that
off a DOL: the stack is a linker's decision and the header does not
mention it. So a value derived from the section table puts the stack
*inside* the arena, and the game zeroes its own return addresses.

The apploader knows, because it is the game's own code. Run it and it
leaves arena low at **zero**, and the game's own start-up then sets it to
0x803A1100 - which is the answer nothing else could have supplied.

Calling a guest function from the host is a return address nothing
occupies and a run until the program counter reaches it. Two things had
to exist first: a handler at the system call vector, because the
apploader flushes caches before installing anything of its own and `sc`
must find something there (`rfi` is what a boot ROM leaves), and a stack,
its very first instruction being a push.

### And pages, which this machine did not translate

The comment in `gc_bus.c` said GameCube software does not use a page
table. This game does. It takes a region no block translation covers -
ten megabytes at 0x7E000000 - installs its own handler at the data
storage vector, and maps pages as they are asked for.

Three things were missing and each looked like a different bug:

- **DSISR was never written.** A handler reads it to learn whether the
  translation was missing and whether the access was a store. Zero tells
  it the translation succeeded, and there is nothing sensible to do with
  that.
- **There was no page table walk.** A handler that mapped a page
  correctly then faulted again on the same address - 205,412 times in
  thirty fields, which reads as a hang.
- **The referenced and changed bits were not written back**, which
  software that reclaims pages relies on.

With all three, the faults drop to **two**: the first touch of each
region, which is what demand paging is supposed to look like.

### Where it stops now, and why that is P5's problem

The game gets through its memory manager, the settings, the audio memory
and the DSP's reset, issues its first graphics commands, and copies a
frame out. Then it waits - for a callback the DSP driver runs when a task
completes, dispatched on mail from the DSP with values 0xDCD10000 through
0xDCD10004.

That is the microcode task protocol, and emulating it is P5's work, not
P6's. Two smaller pieces of the audio hardware were needed to get even
this far and are done: the **sample counter**, which a game starts and
paces itself by whether or not any sound is wanted, and the **mailbox to
the DSP**, whose "a message is waiting" bit must fall because there is no
DSP here to take the message and a game will wait for ever if it does
not.

So the phase order in this document is wrong in one respect, and it is
worth saying: **P6 cannot finish before P5**. A commercial game will not
get to its first frame of content without the DSP answering.

### The tools, which are the part that lasts

None of the six was found by reading code. `gc_run` grew four options and
they are worth more than any of the fixes:

- `--stop-on-bad-access` stops at the first access to an address that is
  not memory and prints the registers **and a stack backtrace**, walked
  from the frame chain rather than from symbols - which is the only option
  a stripped commercial DOL leaves.
- `--watch ADDR` reports every write to an address. "Is this pointer ever
  set?" is a question worth being able to answer in one run.
- `--profile` samples the program counter and names the addresses it sat
  in. This is what found the `sc` loop, after code size and cross-module
  calls had both been suspected and cleared.
- `--stop-at ADDR` and `--stop-after N` stop anywhere, so the backtrace
  can be taken from a running game rather than only from a crash.

`tools/gc/` also has `peekdol.py` in the scratchpad pattern: it
disassembles guest addresses straight out of the disc, which is how each
backtrace was turned into something readable.

## How fast it runs, and the thing that was costing three quarters of it

A window makes a question urgent that a headless runner lets you ignore:
does it run at the speed of a GameCube? It does not, and measuring why found
a mistake that had been there since the interrupts were written.

The machine does a little work between every pair of instructions - moves the
video beam, checks whether the audio interface's counter has reached the
moment a game asked about, sees whether the sound processor owes any mail.
The audio part of that was finding its two registers **by name**, with a
string comparison per I/O block and a linear scan of that block's registers,
eight million times a field.

The measurement was one line: make that function return immediately, and time
a hundred fields of a program that does nothing.

```
100 fields, everything as it was      48.6 s
100 fields, that one function stubbed 11.5 s
```

**Three quarters of the machine's time.** Not the interpreter, not the
graphics, not the memory: a name lookup on a hot path. The fix is what the
video interface's own interrupt registers already did for exactly this reason
- resolve the slot once and keep the index - and it brings the same hundred
fields to **12.9 s**, within a second and a half of the floor the stub
measured. Three point eight times faster, for twenty lines, and every one of
the 26 recorded programs still reports what it reported before.

The indices are reset to a "missing" marker rather than to zero, because zero
is a perfectly good index into the register storage: a stale one would
quietly name the wrong register instead of failing.

**Where that leaves the speed.** About 7.8 fields a second, which is 13 % of
the console's rate. That is the number that matters for P5's gate, and it is
the reason the gate is not met: a 30-second run of the frontend reports audio
underruns, and it does so because the sound device plays 130 ms of audio in
the time the machine produces 17 ms - not because of anything in the audio
path, which its own tests say is right. `--audio-stats` prints the speed
alongside the queue figures for exactly this reason: a report that did not
say how fast the machine went would invite the wrong fix.

**And where the rest of it goes**, measured the same way - stub a piece and
time a hundred fields of a program that does nothing:

| what is left in | 100 fields |
|---|---|
| everything | 12.9 s |
| without the audio, sound-processor and beam hooks | 8.0 s |
| without the three advance calls only | 10.2 s |

So about 8 s is the interpreter and its dispatch, 2.7 s is the three
functions called between every pair of instructions, and 2.2 s is the beam.

Two things were tried and did not help, which is worth recording so nobody
tries them again. Gating the beam's two 64-bit divisions behind "has the
pixel changed" - a pixel is thirty-six cycles, so the answer is no
thirty-five times out of thirty-six - made **no measurable difference**, and
the change was reverted: it added a reset invariant for nothing. And
`gc_set_interrupt`, which looked like another name lookup waiting to be
found, turned out to be two instructions on a field of the machine.

What that leaves is diffuse: roughly thirty host cycles per guest
instruction, spread across a normal interpreter and the call overhead of
three hooks that mostly return immediately. There is no third trick of the
kind the name lookup was. **The recompiler is the lever** - 1.43x on
CPU-bound work so far - and closing a 7.7x gap is its problem rather than
the machine's.

## The frontend

`runtimes/runtime-gc/platform/`, built as `gcplatform`, with `gc_play` for playing a
disc with the interpreter alone. It follows the SNES frontend closely - the
same menu, the same preference file shape, the same pacing loop - because
those are solved and a second solution would only be a second set of bugs.
Three things are the GameCube's own.

**The controller is analogue, and that splits the bindings in two.** Buttons
are bound the way every other console's are: a table of actions, two keyboard
and two controller bindings each, all rebindable. The sticks and triggers are
not in that table, because binding an axis to an axis is not a choice anybody
wants to make - a host controller's left stick is the GameCube's left stick,
its right stick the C-stick, its triggers the triggers. What *is* bindable
about them is the keyboard's way of pushing them, because a key can only be
down or up and a stick pushed by a key has to go somewhere.

**The arrow keys are the stick, not the d-pad.** This is the decision most
likely to look wrong in a diff and is the one that makes games playable. A
GameCube game is steered with the stick and uses the d-pad for menus; a
keyboard whose most obvious keys pressed the d-pad would leave a player
pressing up and watching nothing much happen. The d-pad is on IJKL and the
C-stick on the number pad.

**A stick goes to 80, not to 127.** The GameCube's own stick reaches about
±80 before its gate stops it, and a game calibrated for that treats anything
beyond as full deflection. Scaling a host stick to 127 would mean every push
past two thirds reads the same, and small movements would be coarser than
they need to be. There is a dead zone for the same reason the hardware has
one: a controller at rest is never exactly at rest, and a game reading a
stick that is almost centred walks slowly in a direction nobody asked for.

The sound goes out through a resampler: the machine plays at whatever rate
the game asked the hardware for - 32 kHz for almost everything - and the
device wants its own, so the two are joined rather than assumed to agree. The
queue is held eighty milliseconds ahead and the field rate is steered by up
to half a percent to hold it there, because the sound device's clock and the
performance counter drift apart. A field that produces nothing - a game whose
sound engine has not started - tops the queue up with silence rather than
letting the device run out, which would be heard as a click rather than as a
gap.

`gc_spin` exists for the menu self-test and for nothing else: it sets the
video interface going and waits on the retrace for ever. Every other test
program reports its verdicts and exits, and a machine whose guest has
finished stops advancing its field counter - so "the game runs again" would
fail for a reason that has nothing to do with the menu. It reports no
verdicts, which is why the test runner knows not to ask it for any.

`tools/gc/menu_selftest.py` drives the menu with synthetic events through
SDL's own queue - Escape, then the controller's Guide button, then L3 - and
checks that each opens and closes it, that the field counter stops while it
is open, and that a volume set in it is still there after a restart. It runs
under SDL's dummy drivers against a scratch preferences file and a scratch
save directory, so a player's own settings and memory card are never touched.

### The close button under OpenGL (2026-10-09)

With either OpenGL renderer the window's close button and Alt+F4 did
nothing: the game ran on, and the ways out were the menu's Quit and the
Task Manager. The event loop ended on `SDL_QUIT` alone. SDL makes an
`SDL_QUIT` of a closed window only when it is the last window open, and the
OpenGL path keeps hidden windows of its own for its contexts (`gc_gl.c`),
so there the close arrived as `SDL_WINDOWEVENT_CLOSE` for the game's window
with nothing behind it. With the software renderer the game's window is the
only one, `SDL_QUIT` followed, and it had always worked.

The loop now ends on the close event for the game's own window as it does
on `SDL_QUIT`, by the same way out: the field's thread is closed, the exit
screenshots are taken and the card is written. The other consoles'
frontends open one window each, so they have no such hole.

**Checked.** The menu self-test ends by pushing the close event for the
game's window - the event alone, as OpenGL delivers it, since SDL adds its
`SDL_QUIT` where the system reports a close and not where an event is
pushed - and gives its verdict only when the loop ends on it; a loop still
running two passes later fails ("the window's close button ends the game").
With the branch taken out the step fails. On Shrek 2 in a real window, a
posted `WM_CLOSE` and a posted Alt+F4 each end the game within a second
under OpenGL (fast), OpenGL and Software, with the renderer's statistics
printed on the way out; `main` before this ran on under both OpenGL
renderers until it was killed.

Fifty-two more closes were posted with the settings menu open, on a machine
other sessions were running the same game on. Forty-eight ended at once
(0.2 to 1.2 s where timed). One took 7.2 s over an ordinary shutdown. Two
were still there after six and eight seconds: neither had run a field yet
when the close was posted (one had drawn nothing five seconds after its
window appeared), each took the close when its loop began, and each was in
its shutdown when last seen. One had been minimized by something else and
closed from there. So no close was lost, and a start or a shutdown can
take seconds on a busy machine, which is not this change's: a short
start-to-exit run took a median 2.2 s on `main` and on this, with the slow
ones (4.0 and 5.8 s) at the same moments. What makes them slow was not
found.

### `--fullscreen` is one run's (2026-10-09)

Fullscreen is kept in the settings file (`display.fullscreen`) when it is
chosen in the menu, with the key or by the system, and `--fullscreen` was
meant to be one run's. It was written all the same. The size-change handler
saved whenever the window's fullscreen state differed from the saved one,
and making a window fullscreen for `--fullscreen` is followed by a size
change: the first pass of the event loop wrote `display.fullscreen = 1`,
and the next start without the option was fullscreen.

The handler now writes only when the window is not in the state the
frontend last asked for - a change the system made. The same handler was
in six frontends (GameCube and Wii, Game Boy Advance, Super Nintendo, NES,
Nintendo DS, Nintendo 64) and is changed the same way in each. The menu,
the key and the system still write; `--fullscreen` followed by the key
writes nothing when it turns fullscreen off (nothing was saved) and 1 when
it turns it back on.

**Checked.** Each console's menu self-test driver starts with a run of
`--fullscreen --limit-frames 5` on the new settings file, under SDL's dummy
drivers, where the size change arrives as it does on a screen, and fails if
the file then says `display.fullscreen = 1`. All six fail on `main`'s
frontends and pass on these. On Shrek 2 with a real window: `--fullscreen`
on a new settings file leaves no file and the next start is a window
(`main`: the key written, the next start fullscreen); then the key off
writes nothing, on writes 1, off writes 0.

**Open.** `--scale N` goes into the saved setting the same way
(`f.scale` is the setting and the option overwrites it, so the next save
writes it). Nothing says whether it is meant to be one run's; it is as it
was.

### The menu's Picture line (2026-10-09)

The menu printed "Picture: 640 x 480" whenever no larger picture was on
screen, and at 3x or 4x that read as if the setting had failed. It had not.
A larger picture is made when the game draws a frame; changing Renderer,
Internal Resolution or Anisotropic Filtering restarts the renderer, and
loading a state forgets the pictures, and in the menu the game is paused,
so until Resume the frame on screen is the console's own. The line now says
which of three things is so:

- `Picture: 1920 x 1440 (drawn at 3x)` - a larger picture is on screen;
- `Picture: 640 x 480 for the frame on screen; drawn at 4x from the next
  frame the game draws` - OpenGL (fast) above 1x with none on screen yet;
- `Picture: 640 x 480 (the console's own size)` - Software, OpenGL, or 1x.

Text only: nothing about what is drawn or shown changes. **Checked** on
Shrek 2 in a real window, by posted keys and clicks and a capture of the
window: 3x in play reads the first; choosing 4x in the menu reads the
second, and so does loading a state with the menu open; Resume and Escape
again reads `2560 x 1920 (drawn at 4x)`; OpenGL reads the third.

### The screenshot key saves what the window shows (2026-10-09)

The screenshot key (F12 by default) wrote the display buffer's picture
whatever was on screen: a 640 by 480 file of a window showing 1920 by 1440.
It now writes the larger picture when one is on screen
(`screenshot_larger`, which `--internal-shot` already used) and the display
buffer's otherwise - Software, OpenGL, 1x, and the paused moments the
Picture line describes above, where asking for the larger one finds none
and the key falls back. `--screenshot-prefix`, which the hub makes covers
with, is as it was: the display buffer's picture, the same size whatever
the setting. The file is a BMP of 11 MB at 3x and 20 MB at 4x. A Wii game
started in 16:9 keeps the console's pixels in it, a quarter narrower than
the picture, as the 640-wide file does.

**Checked** on Shrek 2 in a 640 by 480 window, F12 after three seconds of
play: 1920 x 1440 at 3x, 2560 x 1920 at 4x, 640 x 480 at 1x, under OpenGL
and under Software; 640 x 480 right after choosing 4x in the menu, and
2560 x 1920 once the game had resumed. `main` wrote 640 x 480 at 3x.

### A Window Size that fits the screen (2026-10-09)

Window Size was applied as chosen, whatever the screen: "3x (1920x1440)" on
a screen of 1440 lines made a window whose title bar was 31 lines above the
top of the screen and whose last 48 lines of picture were under the
taskbar, and 4x was worse. Nothing said so, and the close button was out of
reach.

The size in use is now the largest one up to the setting whose window -
title bar and frame included (`SDL_GetWindowBordersSize`, or Windows' usual
31 and 8 before the window exists) - fits what the system leaves free of
the screen the window is on (`SDL_GetDisplayUsableBounds`):
`fitted_window_scale`, used where the window is made and where the setting
is applied. The window goes in the middle of that free part, on the screen
it is on (it used to go to the middle of the first screen, taskbar or not).
The saved setting is not changed, so the same file gives 3x on a screen
that has room for it, and the menu says when the two differ: "3x
(1920x1440) does not fit this screen: choosing it gives 2x (1280x960).
Fullscreen uses the whole screen." The Nintendo DS frontend already fitted its layouts
this way (`fitting_scale`).

A window the player drags larger is left alone, as is fullscreen. What
this costs: with Window Size 3x and Internal Resolution 3x saved, the
window on a 1440-line screen is 2x and shows the 3x picture scaled down
until the player goes fullscreen, which is where the line points.

**Checked** on Shrek 2 on a 3440 by 1440 screen with a 48-line taskbar and
a second screen of 1366 by 768. Window Size 3x saved: the window is 1296 by
999 at (1072, 196), wholly inside the free 3440 by 1392 (`main`: 1936 by
1479 at (752, -31)); the same for 4x, for 3x chosen in the menu, and for
leaving fullscreen in a game that started in it. Moved to the second screen
and given 3x there, the window became 1x in the middle of that screen's
free part. The setting stayed 3 in the file throughout.

**Not done.** The Game Boy Advance, Super Nintendo, NES and Nintendo 64
frontends apply their Window Size as chosen (by reading them; not run):
Super Nintendo 7x and 8x, NES 6x to 8x and Nintendo 64 6x are 1440 lines or
more, Game Boy Advance 8x is 1280. Linux and macOS were not run; where SDL
cannot give a window's frame the guess is used.

### The menu says what goes with what (2026-10-09)

Three things about the picture settings were true and nowhere to be read.
Each is now a grey line under the setting it belongs to, text only.

**Under Internal Resolution**, how the lines drawn meet the lines shown
(`draw_fit_note`: the larger picture's height against the viewport's):

- `Shown pixel for pixel (1920 x 1440).`
- `Scaled down to fit: 1920 lines drawn, 1440 shown, which is softer. 3x
  fits this size exactly.` - 4x on a 1440-line screen. The larger picture
  is fitted with one smooth sample, so this is softer than 3x there and
  not sharper; under half size the sample skips lines and the line says
  that instead.
- `Scaled up to fit: 480 lines drawn, 960 shown. 2x fits this size
  exactly.`
- `Shown line for line (1440 lines); across, 1920 pixels are fitted to
  3440.` - Stretch, or a Wii game in 16:9, whose pixels are not square.

The setting that would be one to one is named when there is one among 1x
to 4x. There is no line while the larger picture is still to come (the
Picture line says so). The help text above it gained a sentence: flat art
- the HUD, text, menus - keeps the game's own resolution, and a texture
pack is what replaces it.

**Under Scale Filter**, while a larger picture is on screen: `For the
console-sized picture only: one drawn larger is always fitted smoothly.`
Nearest was accepted there and changed nothing.

**Under Renderer, Internal Resolution and Anisotropic Filtering**, when
`--renderer`, `--internal-scale` or `--anisotropy` chose a value for this
run that is not the saved one - the hub's own settings start a game that
way: `Chosen for this run by whatever started the game; the saved setting
is Software.` The combo showed the run's value as if it were the setting.
Choosing in the menu replaces both, as before, and the line goes.

**Checked** by reading captures of Shrek 2's menu: each of the four lines
(a 2x window at 1x, 2x, 3x and 4x; fullscreen on 3440 by 1440 at 3x and 4x;
fullscreen with Stretch; a 640 by 480 window at 3x for "skips lines"), the
Scale Filter line at 3x and its absence at 1x, and the three lines of a
run started with `--renderer fast --internal-scale 2 --anisotropy 4` on a
file that says Software, 3x and Off. A Wii game in 16:9 was not run.

### A settings file shared between builds (2026-10-09)

Every GameCube and Wii game reads and rewrites the same `gc_prefs.ini`,
and each game is a build of its own: it carries the frontend of the day its
player last rebuilt it. `save_prefs` wrote the file from the settings that
build knows, so a game built before a setting existed erased it for the
others whenever anything was changed in its menu. A game built before
Fullscreen was kept does that to `display.fullscreen`: the line was gone
after one press of the FPS key in such a build.

`load_prefs` now keeps each line it has no name for - a setting, or a
binding for a control this build does not have - as it was read
(`prefs_other`), and `save_prefs` writes them back after its own, under a
comment line. Comments and lines without `=` are not kept. The line buffer
grew from 256 to 2048 characters so that a setting that is a path is not
cut. A binding for a control this build does have, with a value it cannot
read, is dropped as before: that setting is this build's.

**What it does not do.** It cannot reach builds that already exist: a game
built before this erases what it does not know until it is rebuilt. A line
nobody knows any more is carried for ever. Two games open at once still
write the whole file each, and the last to change a setting wins. The
other consoles' frontends, whose games share their settings files in the
same way, are not changed.

**Checked.** `tools/gc/menu_selftest.py` starts its settings file with a
setting and a binding no build has, and fails unless both lines are there,
as written, after the self-test has saved its own settings many times
over; with `main`'s `gc_play` both are lost. On Shrek 2: a file with four
such lines, one of them 300 characters long, the FPS key pressed twice
(two saves): all four kept, once each, and the known settings and
bindings read and written as usual; `main` lost all four.

## The sound microcode

`gc_ax.c` is what the microcode a game uploads would have done. A game hands
over a command list five milliseconds at a time; the list primes nine mixing
buffers, points at a linked list of voices, asks for all of them to be mixed,
and writes the result where the audio engine will find it.

**This is the least well-evidenced thing in the machine and it is worth
saying so plainly.** Everything in it comes from `docs/gc-dsp-ax.md` parts B
and C, which were compiled from a second implementation read as prose. There
is no `gxtest` for the sound processor, no captured output, no hardware to
ask. The tests check that the code does what the document says; nothing
checks that the document is right.

One structural check was available and it did pass. The parameter block's
field order is given as a list rather than as offsets, so the offsets here
are the running sum of the field widths - and that sum closes at 122
halfwords, which is exactly the 244 bytes the block is independently known to
be. A field of the wrong width would not have landed there.

**Three things about the format would each give something that runs and
sounds wrong.** A sample address is not a byte address: it counts units of
the access size, so ADPCM addresses count *nibbles* and sixteen-bit ones
count halfwords. There is no fixed ADPCM coefficient table - the sixteen
coefficients are per-voice, carried in the parameter block, fitted to that
sound when it was encoded, so a decoder written against a constant table
decodes noise. And a command's argument count is encoded nowhere, so the
walker has to know the table and an opcode it does not know has to stop the
walk rather than guess a length.

**And one thing the tests caught that reading had not.** The resampler's
fractional position is a sixteen-bit field of the parameter block, and the
first version added the step at the end of each output sample rather than
consuming it immediately - so whatever was left above the point when a
millisecond ended was thrown away. That is one input sample dropped in every
thirty-two: a sound playing very slightly fast, invisible to anything that
only checks that sound came out. What found it was a test that counted how
far the sample address had advanced after a hundred and sixty samples and
compared it against the frame arithmetic worked out by hand.

The same test file also fixed itself twice, which is worth recording because
both were the test being wrong rather than the machine. A voice's first
output comes from the four history samples in its block rather than from its
data - the fraction is sixteen bits, so it can never start at a whole step,
and a game primes the history when it starts a voice. And a sample passes
*two* volume stages, the envelope and the send, so a value at unity comes out
two counts lower than the arithmetic for one stage predicts.

**What a commercial game actually asks for.** Shrek 2 sends 1327 command
lists in 400 fields and every one of them walks to its END with the argument
counts in step, which is the check that the command table is right - one
wrong count and the rest of a list is garbage opcodes. The seven commands it
uses are `SETUP`, `SET_MAIN_LR`, `SET_VOICE_LIST`, `PROCESS`, `COMPRESSOR`,
`OUTPUT`, `END`.

`SET_MAIN_LR` was on the "skip it" list and should not have been: it is where
a game's *own* mixed material arrives - streamed music, decoded on the
processor and handed over finished - and it **replaces** the main mix rather
than adding to it. A machine that skips it plays the sound effects, none of
the music, and leaves whatever `SETUP` primed underneath everything. It is
implemented now. (In this particular run its buffer is silent, so the output
is byte-identical either way; the game has not started streaming two seconds
in. That is a reason to have found it by reading the trace rather than by
listening.)

**What the voices look like, which is the best evidence available.** The
trace of the first few playing voices:

```
voice fmt 0000 src 1 ctl 0001 ratio 0000b066 cur 0012827e end 001581ff loop 1 vol 8000 sends 8000 0000
voice fmt 0000 src 1 ctl 0002 ratio 0000b066 cur 000082fc end 000381ff loop 1 vol 8000 sends 0000 8000
```

A stereo pair - one voice routed to main L, one to main R - of looping ADPCM
with linear resampling. And the ratio decodes to 0xb066 / 65536 x 32000 =
**22050 Hz exactly**, which is a real sample rate and not a number that comes
out of a field being read wrongly. That, together with every list parsing, is
as close to a check as there is without hardware.

One thing that is *not* settled by it: the doc says the volume envelope is
signed on this console, and this game writes `0x8000` for both the envelope
and the send. Signed, that is two inversions; unsigned, it is two unity
gains. They give the same answer here, so this game cannot tell them apart -
and nothing else read so far can either.

(The compressor, the auxiliary busses and four corrections to what is above
came later: see [Clean sound](#clean-sound-2026-09-15).) What is deliberately
not here: the polyphase resampler, whose coefficients
live in the sound processor's data ROM and are not ours to ship - it degrades
to linear, which is what a second implementation without the ROM does too.
The initial time delay, the de-pop ramp and the Dolby paths are not
implemented and are marked in the doc as things nothing establishes well
enough. And the oldest microcode revision's completely different mixer
encoding is not implemented: a machine that applied the modern table to it
would route almost everything wrongly, so being wrong in a way that is
written down is better.

## Where sound leaves the machine

Before anything makes a noise there is a clock, and the clock is worth
having on its own. The sound processor mixes into a buffer in main memory
and a separate engine walks that buffer out to the converter thirty-two
bytes at a time, loops back to the start, and raises an interrupt each time
it gets there. A game's audio thread does its work in that interrupt. So
the rate it arrives at is not the rate the *sound* runs at - it is the rate
the **game** runs at, and a machine that gets it wrong produces something
that does not sound wrong so much as behave wrong: a mixer that starves, a
loop waiting on a buffer that never fills, work paced at half speed.

`gc_audio.c` is that engine. It plays whatever is in the buffer, which for
now is silence, into a ring the frontend will take frames from.

**The rate bit is inverted, and it is in the other device's register.** It
lives in the audio interface's control word rather than the engine's own,
and clear means 48 kHz while set means 32 kHz. Read it the natural way round
and everything is out by a factor of one and a half in the direction that
still looks plausible.

**And the engine does not stop at the end of a buffer.** It reloads both the
address and the count from whatever the guest last programmed and plays on.
That is what makes the double buffering work: the interrupt says "the buffer
you gave me has been played", and a game that has not yet handed over the
next one hears the previous one again - a repeated fragment, which is the
right failure and not a silence.

`gc_audiotest.c` checks the three things that fail separately: that the
interrupt arrives at all, that a hundred of them arrive in half a second (a
160-frame buffer at 32 kHz is five milliseconds, and a factor of two cannot
hide in that window), and that the remaining-bytes register moves and stays
inside the buffer. The last of those needed the test corrected rather than
the machine: a block is a quarter of a millisecond and the first version
polled the register tens of thousands of times faster than it changes, then
reported it stuck.

What is not here yet is the thing that fills the buffer: walking the sound
processor's command list, the voices, and ADPCM decoding.
`docs/gc-dsp-ax.md` parts B and C are the reading for it, and part E puts it
in order.

## Clean sound (2026-09-15)

A play-test of the hub build of main (`b0f2212`, OpenGL) reported "some
static and latency". Everything below was measured on real output - the
runner's `--dump-audio` (what the machine played, 32 kHz) and the frontend's
device stream through SDL's disk driver or the real WASAPI device - never on
counters alone. The machine was shared with three other agents' Shrek 2 runs
throughout (CPU 90-100 % for almost every run; each comparison below is back
to back, and says so).

### What the static was

**Mostly the frontend, not the mix.** The machine's own output was already
clean in the measures that catch static. Over 100 s of the gameplay script
(`play.txt`: title, New Game, storybook, cutscene, swamp), main's mix had no
runs at the rails (14 samples touched them, in the menu's sparkle effect),
median spectral flatness 0.0016 (title), 0.0007 (narration), 0.0022
(gameplay) over 64 ms windows - white noise is 0.56 - and 0.01-0.02 % of its
power above 10 kHz. What a listener heard as static came after it:

- **The device ran dry**, 174-303 times a minute and a half, whenever a field
  was late - and SDL's queue has no way to end a sound gracefully, so each gap
  was a step from the last sample to zero and back. In a 60 s capture of the
  device stream: 1.2 gaps a second, 13-18 of them with a hard step, 0.43
  clicks a second.
- **The queue grew into latency.** The pacing loop paid back a debt of up to
  250 ms by running flat out; everything played in that time went into the
  queue, and the 0.5 % steer took most of a minute to drain it. After the
  menu's load the WASAPI queue sat at 223 ms (plus a 21 ms device buffer and
  SDL's own) and was still 121 ms 45 s later.

The mix did have four real errors, all audible only as polarity, level and
the odd click - found by tracing every voice Shrek 2 starts and comparing
with the reference:

| what | main | why it matters | Shrek 2 |
|---|---|---|---|
| send volume | signed | **0x8000 is unity**; signed it is -1.0 | music sends at 0x8000, effects at 0x7bc0: opposite polarity |
| end of a one-shot | re-decodes the last nibble for the rest of the millisecond | the predictor runs on its own output: a buzz | effects end mid-millisecond |
| de-pop order | send order | L,L,L / R,R,R / S,S,S; the library ramps them out via SETUP | this game's library never primes SETUP |
| SET_MAIN_LR / UPLOAD_LRS / aux buffers | 16-bit | 32-bit, as the mixing buffers | this game hands over zeros |

and the compressor and auxiliary busses were not implemented (Shrek 2's list
is `SETUP, SET_MAIN_LR, SET_VOICE_LIST, PROCESS, COMPRESSOR(0x8000, 16
frames), OUTPUT, END` every frame; a threshold of 0x8000 only fires above full
scale, so it changes almost nothing here). Measured on the same 100 s: samples
at the rails 14 -> 0, clicks 211 -> 181, frames 600-3,700 byte-identical.

### The mix (`gc_ax.c`)

Sources: Dolphin's DSP-HLE `UCodes/AX.cpp`, `AXVoice.h`, `AXStructs.h`, `AX.h`
and `DSP/DSPAccelerator.cpp` (GPL-2.0, master), read as a behaviour reference
and restated - no code taken; `docs/gc-dsp-ax.md` for the rest.

- Sends are unsigned with a wrapping delta; the envelope stays signed (the
  reference's GameCube case); the LPF's input coefficient is unsigned.
- The sample accelerator is modelled as a device loaded from the block each
  millisecond: the header is read as soon as the address reaches a frame
  boundary (so a block written back mid-sound holds the next frame's
  predictor), the end is an equality test after the read, the two header
  end cases are carried, and an end stops reads - zeros - until yn2 is
  written, which the loop handler does. The written-back address is the loop
  start, as the hardware leaves it.
- Every millisecond's updates are applied whether the voice runs or not (an
  update can start it), from the array read once with the block.
- Linear resampling steps first and weighs the two oldest history samples;
  nearest takes decoder samples straight, ignoring the ratio.
- COMPRESSOR: if any main L/R sample of the frame is over the threshold the
  frame is scaled by the attack ramp indexed by the release position, which
  restarts at its full length; otherwise a running release steps down and
  uses ramp 11 + position. Ramps are 160 unsigned Q15 gains. The position is
  in the state (a state from before loads with it idle).
- MIX_AUXA/B, MIX_AUXB_NOWRITE, MIX_AUXB_LR, SEND_AUX_AND_MIX and
  DOWNLOAD_AND_MIX upload and download their 32-bit buffers.

`gc_ax_test` has a case for each (21 checks that all fail on main's
`gc_ax.c`). Still not here: polyphase resampling (the ROM's tables; linear
instead, and Shrek 2 asks for linear), the initial time delay, Dolby Pro
Logic II routing, the oldest microcode's mixer encoding.

### The frontend (`gc_audio_out.cpp`, `gc_sdl.cpp`)

Sound now goes through `SDL_AudioStream` (at the machine's actual rate,
`gc_audio_rate`) into a ring the device's callback reads:

- **A gap is faded.** When the ring runs out, the last 3 ms slope to zero;
  the callback then waits for a steady amount (the target) and fades back in
  over 3 ms. Fewer, cleaner gaps instead of a stutter of one-field fragments.
- **The target is 40 ms** measured just after a push (a field and a callback
  below it before the next push), raised 8 ms at a time only when the device
  runs dry while the machine has kept up for 30 fields, never past 104, and
  eased back 4 ms after 20 s without that.
- **The device is asked for 512 frames and may choose**; WASAPI picks its 10 ms
  period (480). Main asked for 1024 and got a 21 ms buffer on top of SDL's.
- **Pacing is a clock**: the field rate is steered 0.5 % fast to 2 % slow by
  the ring's smoothed error; the "sound is short, so run the next field at
  once" rule is gone (it tied the clock to the ring that follows the clock);
  a debt over six fields is written down to six. Writing it off entirely was
  tried first: a back-to-back 90 s run came out at 46.7 fields a second
  against main's 55.7, because the field after each write-off slept even
  though the machine was behind.
- `--audio-stats` reports the ring, the dry count and silence, the latency
  before the system mixer and how pacing went; `--pace-log FILE` writes a
  line a field.

**Latency.** The default WASAPI endpoint here (48 kHz, probed with
`IAudioClient`, shared mode) has a 10 ms period and a 22 ms endpoint buffer,
and reports no stream latency. So before the system mixer:

| | main | this branch |
|---|---|---|
| 30 s windowed, WASAPI, muted, no input | queue 60.9 ms + 21.3 ms device buffer (+ SDL's buffer to the 10 ms period) | ring 38.2 ms + 10.0 ms = **48 ms** |
| 90 s, same | queue 85.1 + 21.3 ms | ring 48.7 + 10.0 = **59 ms** (target raised to 52 once) |
| 45 s with `play.txt` | queue peaked 223 ms after the menu load, 121 ms at the end | ring 37-57 ms per 300-field window |

End to end that is about 48-59 ms plus up to a period of endpoint buffer and
the machine's half-field ring: 60-80 ms, against main's 100-170 ms steady and
250+ ms after a load.

**Dropouts** (60 s of `play.txt`, OpenGL, SDL disk driver, back to back, two
rounds; the disk driver's clock runs slow, so its queue figures are not
latency):

| | main | this branch |
|---|---|---|
| gaps (zero runs >= 2 ms next to sound) | 71-75 (1.20-1.25 a s) | 27-28 (0.45-0.47 a s) |
| gaps with a hard step (> 1000 next to the zeros) | 13-16 | 0 |
| clicks | 25-26 (0.42-0.44 a s) | 10 (0.17 a s) |
| speed | 55.3-55.5 fields a s | 55.0-55.2 |

### P5 windowed runs (WASAPI, `--mute --audio-stats`, OpenGL, no input, back to back)

| | main | this branch |
|---|---|---|
| 30 s | 1,800 fields, 60.0 a s, ran dry 0 | 1,798 fields, 59.9 a s, ran dry 0 |
| 90 s | 5,029 fields, 55.8 a s, ran dry 303 | 5,015 fields, 55.7 a s, ran dry 84 (6.4 s of silence) |

CPU load was 100 % from other agents through all four; the 90 s run's shortfall
is fields 2,400-3,000 and 4,200 on, where the machine did 33-70 fields a second
of work.

### The "loading screen at 55.9-57.2 paced" figure

Not a pacing bug. The P5 table's "loading, 600-2,400" window averages a
stretch the machine cannot do in real time with one it does easily: with
`play.txt`, fields 600-900 (the menus' load, where the audio engine is
stopped and the output is digital silence) took 18-20 ms of work a field -
**50-54 fields a second unpaced**, in a window (`--unthrottled --pace-log`)
and paced alike - while 900-2,400 did 90-116. Pacing cannot bank the fast
windows' surplus for the slow one (nor should it), so the paced average is
1,800 / (300/52 + 1,500/59.94) = 58, and a slower 600-900 gives the table's
56-57. Every 300-field window the machine can do at 60 was paced at 59.9-60.0
by main's loop on WASAPI too; main's real pacing faults were the queue growth
and catch-up bursts above. **Unverified:** these were measured under
90-100 % load from other agents; whether 600-900 stays under 60 on a quiet
machine is for the speed work to say (it is game code, not sound).

### The standing checks

- `gc_ax_sound` (CTest): three seconds of a looping stereo PCM stream, the
  same stream on a second pair at half volume, and an ADPCM effect that starts
  and ends three times, with noise in main memory under the sample addresses.
  Held to: no runs at the rails, at most 3 clicks, median flatness <= 0.05,
  <= 0.5 % above 10 kHz, and the music's level within 1.5 dB of what its
  volumes give. With samples read from main memory (the static before #32) it
  fails 4 checks (1,737 rail runs, flatness 0.37, 8.9 % above 10 kHz, +14.5 dB);
  with main's `gc_ax.c` it fails the level (-9.5 dB: unity sends inverted).
- `run_checks` audio stage: the frames run now also dumps its sound, and
  `tools/gc/audio_check.py` checks Shrek 2's title music (2-15 s), logos
  (23-30 s), the silent load (40.5-44 s, every sample zero) and the menu music
  (46-61 s): level -45 to -12 dBFS, flatness <= 0.05, <= 0.5 % above 10 kHz,
  <= 2 drops to digital silence, and over the whole capture <= 5 rail runs and
  <= 4 clicks a second. This branch: 0 rail runs, 0.62 clicks a second,
  flatness 0.0013 / 0.0014 / 0.0023, 0.01 % high band. A scratch build with
  the ARAM fix reverted fails 11 of the 16 checks: 12,508 rail runs, 43.6
  clicks a second, flatness 0.15-0.28, level -10 dBFS.

### Gates

`run_checks --quick` (build, 20 GC and hub CTests, every recompiled program,
Shrek 2 built in 46 s, frames 600-3,700 identical to main's reference, audio)
passed in 195 s wall; `--differential --auto-input` agreed at every field end
over 1,600 fields (0-800, then 800-1,600 from a state), with command lists
still arriving after New Game (4,997 at 1,500, 5,331 at 1,600). The state hash
changes, as it must: the mix writes its output and the voices' blocks into
main memory, which the game reads.

### Left open

- A frontend capture check. The device path is only measured by hand here
  (disk driver and WASAPI); a check of it would be as noisy as the machine's
  load.
- The menu's "sparkle" effect and the gameplay's 2-second-periodic spikes
  (single samples ~3,000 above the waveform) look like content, but nothing
  here proves it; a hardware capture would.
- Polarity of the whole mix rests on the reference's signed envelope: Shrek 2
  writes 0x8000 there, so its entire output is inverted relative to an
  unsigned reading. Inaudible on its own; unverified.

## The memory card

A flash chip on the expansion bus that answers three commands worth having -
read 512 bytes, program 128, erase 8192 - and a file system built on top of
them by the save library running on the guest. Only the chip is here.
`docs/gc-memory-card.md` is where every number came from; it was written
first, from libogc's `card.c` and `exi.c`, and the implementation is short
because the reading was long.

The image is a plain file of the card's size - a new one is 524 288 bytes, a
4 Mbit card, the smallest size the library accepts - so it is the same thing
every other tool means by a memory card image. An existing image may be any
of the six sizes (4 to 128 Mbit) and the card reports that size. `gc_run
--card FILE`, and a generated game's headless runner with the same option,
put one in the first slot; a file that is not there (or is empty) is
formatted, because a game asked to save on a machine that has never saved
should find a blank card rather than a demand to go and make one. A file of
any other size is somebody else's and is refused rather than overwritten.

**Four things in it would each have produced a card that looks like it
works.** Three came out of the reading and one out of running it.

*The first byte of a transfer is the most significant byte of the immediate
register*, in both directions. The status byte is a one-byte read, so it has
to be presented in bits 31:24. Put it in the low byte and it reads back as
zero, which the library reads as a *locked* card - and answers by uploading a
decryption microcode to the sound processor and running a challenge-response
cipher, which fails anyway. The symptom is a mount that takes a very long
time and then reports an I/O error.

*A write finishes by the card asking for attention, not by the transfer
completing.* The program command's data phase completes, the bus is released,
and then the card raises its own interrupt to say the flash was actually
programmed. A card that skips that reads perfectly and silently never saves:
the write appears to succeed and fails a tenth of a second later, when a
watchdog nobody was watching fires.

*The settings block gates the mount.* Two bytes of it are a checksum over
each slot's flash identifier, and the library refuses to mount a card at all
unless they agree - before it reads a single block. Ours were zero and had to
be `0xff`, which is what the complement of an all-zero identifier is. This
was in the research, not in the debugging, and it would have been a miserable
thing to find afterwards: everything else works and the card will not open.
The same reading found that the flags byte was setting *boot into the
console's own menu* rather than *stereo sound* - the bit was simply the wrong
one - which is now fixed too.

*And the transfer-complete interrupt is separate from all of that.* A bulk
read is waited for by the transfer-complete interrupt and the short commands
by spinning on the start bit, so a machine that clears the start bit and
stops answers the short commands and hangs on the first block read. That was
the one found by running it: the mount printed nothing and the program sat in
its idle loop for ever.

The two are gated by mask bits that sit exactly one place below them - bit 0
masks bit 1, bit 2 masks bit 3, bit 10 masks bit 11 - which is why the check
for "should this interrupt be raised" is a single shift.

**The written image is checked from outside.** After `gc_cardtest` runs, the
file on disk has a directory entry naming the game and the file, an
allocation table whose newer copy says fifty-eight blocks free with block 5
ending a chain, and block 5 holding the payload byte for byte. That is the
check that this is a memory card and not merely something this machine can
read back.

Two tests, because they answer different questions. `gc_cardtest` drives the
library - probe, mount, create, write, *unmount, mount again*, read - and the
remount is the point: without it the test would pass against a card that
never wrote anything, because the library answers out of its own work area.
`tests/gc/gc_card_restart.py` runs the same program twice against one image
and insists that the first run reports an empty card and the second finds the
file the first one wrote. Both halves are required, and a run that reports
neither fails - a check that passes by finding nothing would also pass if the
card were never written at all.

The image is written back a field after the last change rather than after
every one. A save is an erase and sixty-four page programs for each of the
data, the table and the directory; writing half a megabyte back after each
would be a hundred megabytes of file I/O for one save, and waiting for the
burst to stop costs one. It is written to `<card>.tmp`, flushed to the disk
and renamed over the card (see below for why).

What is deliberately absent: the unlock cipher (the card reports itself
unlocked and is never asked), insertion and removal (a card that is there
from boot and never leaves needs none of it), and slot B.
`docs/gc-memory-card.md` part G lists them with the reasons.

### A real game's saves (2026-09-15)

Every check above was a test program, and every test program links libogc.
Shrek 2 links the retail CARD library, and the first time anybody played
through its own save menus - New Game, *Save* - it said **"The Memory Card
in Slot A is corrupted and needs to be formatted."** on a card this machine
had just formatted.

**The retail library checks the card against the console, and libogc does
not.** On every mount the retail `VerifyID` re-runs the format's generator
from the format time stored in the header (a signed 64-bit time: multiply by
1103515245, add 12345, arithmetic shift right 16, twice per byte, the second
masked to 15 bits) and requires each of the twelve serial bytes to equal the
console's flash ID for that slot plus the generator's byte; anything else is
`CARD_RESULT_BROKEN`. (Read in the decompiled SDK's `CARDCheck.c` as a
behaviour reference, not copied; the same check also wants device 0, the
right size and header checksum, and encoding 0.) Our formatter wrote a
serial hashed from the card's path with a format time of zero, and the
settings block reported a flash ID of zero - so the check wanted a serial of
twelve zeroes and found the hash. libogc's mount never looks, so
`gc_cardtest` and `gc_card_restart` passed throughout.

The fix is the console's, not the formatter's. On hardware the settings
memory learns the flash ID from the card: a card is locked at power-on, the
unlock handshake derives the ID from the chip, and the library writes it into
the settings block with its checksum. This card reports itself unlocked, so
the handshake never runs; the machine now does its part instead.
`gc_card_flash_id()` recovers the ID from the card's own header (each serial
byte minus the generator's byte) and the settings block reports it for slot A
with the matching checksum at `0x3a` (slot B, always empty, stays zero and
`0xff`). Any self-consistent card now mounts - one this machine formatted,
one the game formatted, one copied from another save directory or made by
another tool at another size - which is what a real card moved between
consoles does. An empty slot leaves the settings block exactly as it was, so
runs without a card (every frame reference) are unchanged; with a card the
settings' flash ID now varies with the card, and so does the settings
checksum under our wide rule (the question of the narrow rule in
`gc-memory-card.md` F.3 is untouched by this).

Before the fix, a card formatted *by the game* after the "corrupted" prompt
worked everywhere (its serial was built on the same zero flash ID), and a
card formatted by this machine never worked anywhere. Old cards of both kinds
mount now.

**The scripted flow.** The generated game's headless runner had no card at
all, so `--card FILE` was added to it (`gc_runner.c`, clearly separate; a
`--differential`'s interpreter and the locate passes get the same card in
memory only, `gc_card_insert_unbacked`). `tools/gc/scripts/shrek2_save.txt`
drives, from boot with a fresh card:

| Field | Press | Screen |
|---|---|---|
| 3000 | A | main menu: New Game |
| 3660 | A | "Do you want to save your new game?" - Save |
| 4260 | A | "No Shrek 2 game data present... create a save file?" - Continue (the file is created, blocks 5-6, field 4348) |
| 4860 | A | "Creation successful." |
| 5100 | A | "Save New Game": Game A (written at field 5188) |
| 5400 | A | "Save successful." |
| 5700, 5820 | A | skip the story book and the opening cutscene |
| 6300 | START | in the swamp level: pause |
| 6480-6540, 6600 | DOWN x3, A | Save Game |
| 6780, 6840 | DOWN, A | Game B (written at field 6928) |
| 7080 | A | "Save successful." |

and the run ends (the quit). `shrek2_load.txt` then boots again against the
same card: DOWN, A at 2940/3000 (Load Game; the game had already read the
whole card and its file at field 17), DOWN, A at 3300/3360 (Game B), and the
screens are "Select Game" with Game A and Game B both "0% Complete" (Game C
Empty), "Load successful.", then "Contents" with Chapter Select, whose first
chapter, Shrek's Swamp, is open. A whole save - three erases and 192 page
programs, data, table, directory - lands inside one field, because a
transfer completes in the instruction that starts it; the game holds its
"Saving..." page for its own sake.

Checked in all three places, frames looked at each time:

- **The runner**: save run 72 s, load run 53 s (to field 4500, Chapter
  Select) when the machine was quiet; 343 s and 41 s under three other
  agents' Shrek builds.
- **The windowed frontend** (`--play-input --save-dir --renderer opengl
  --unthrottled`, dummy SDL video, so the renderer fell back to software):
  the same card operations at the same fields; screenshots at field 7060
  ("Save successful." over the paused level) and, relaunched, 3340 ("Select
  Game", Game B 0% Complete) and 3600 ("Load successful."). The load runs
  left the card byte-identical.
- **The hub's launch path**: a scratch `--data-dir` inside the build tree
  (`--batch --import` of the disc, 77 s with the cover), the game's per-game
  `launch_args` set to the scripts, and `--launch shrek_2`, which runs
  `games/shrek_2/game/shrek_2.exe` from its own folder with no `--save-dir`
  exactly as the hub does. `shrek_2.card` landed beside the executable with
  the save on it; `--batch --rebuild-all` (33 s) kept it byte for byte; the
  relaunched load showed Game B.

**Edge cases**, each from boot, frames looked at:

- *A blank card* (524 288 bytes of `0xff`, never formatted) and *a 2 MB file
  of zeroes*: "corrupted and needs to be formatted" with Continue Without
  Saving / Retry / Format Memory Card / Cancel. Taken through on the 2 MB
  file - Format Memory Card, then Yes - the game erases and programs the five
  system blocks, creates its file in the same field and says "Format
  successful."; the result is a 16 Mbit card with 251 blocks and a serial
  built on a real format time, which then mounts from another save directory
  and goes straight to "Save New Game".
- *A corrupted card* (bytes flipped in the header and in both directory
  copies of a card with a save on it): the same "corrupted" prompt; no crash,
  nothing written.
- *A file that is not a card* (100 000 bytes): refused, the runner says so
  and exits, the frontend runs without a card (the game says there is none),
  the file is untouched.
- *A card from another save directory* (serial seeded from a different path):
  mounts and loads - that is how every load run above worked.
- *Saving while a save state exists*: loading a state writes the state's card
  back, so in-game saves made after the state are gone from the card (seen in
  an early exploration: a state from before the save reverted the card to
  blank). That is still right for the game, whose memory holds the state's
  directory, but it lost saves silently; the replaced image is now kept in
  `<card>.bak`.
- *The process killed hard right after a save*: the frontend was
  `Stop-Process -Force`d the moment the in-level save's directory update was
  on disk (105 s in); the card held both saves, with no temporary left.

**Durability.** Write-back used to open the card for writing in place, which
truncates it first: a crash, a kill or a full disk between the truncation and
the last byte left a short file, which the next run refuses as not a card -
every save on it gone, where a console loses at most the one being made. It
now goes through `<card>.tmp`, `fflush` and `_commit`/`fsync`, then
`MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)` / `rename`, so the file is
always one whole image. A write-back that fails (another process holding the
file) is reported once and retried a second later instead of every field.
Writing through on every program was considered and not done: it is the
hundred megabytes above, and the idle-field write already puts a save on disk
sixteen milliseconds of machine time after it ends - the kill test shows it
arriving before the game has finished showing its "Saving..." page.

**Tests.**

- `gc_card_retail_mount` (`tests/gc/gc_card_test.c`) drives the card
  through the EXI registers and mounts it under the retail library's checks -
  identifier, unlocked status, the slot's flash-ID checksum, the serial
  against the flash ID and format time, system-block checksums, free counts -
  saves the way the library does (erase, program by pages, the older table
  and directory copies, the card's interrupt after each), restarts and
  remounts; also a 16 Mbit card formatted with another console's flash ID
  and a real format time, a file that is not a card, a card in memory only,
  and the `.bak` a state load leaves. Broken three ways in a scratch build it
  fails each time: the settings' flash ID back to zero ("header serial byte 0
  is bc, the console's flash ID says 00"), no interrupt after a program, and
  a write-back that does nothing.
- `tools/gc/check_card_saves.py` is the same thing for a real game and needs
  the disc, so it is not a CTest: a save script must leave a card that mounts
  with a file with the expected game code; a load script must leave the card
  unchanged and show a different picture at a given field than the same
  script against a blank card ("Load successful." against the main menu,
  mean difference 17.5). Shrek 2: PASS in 436 s.
- `run_recompiled_tests.py` now gives `gc_cardtest` a card (it had been
  running with the slot empty); recompiled and interpreted agree.

**Gates.**

- `run_checks --quick --build build-card` with the speed agent's frame
  reference: build 5 s, CTest (20, including the new one) 219 s, recompiled
  221 s, game build 75 s, frames 69 s, identical at 600, 1200, 2000, 2500,
  3000 and 3700; 226 s wall. The frame reference has no card, so it is
  untouched by the settings change by construction.
- `--differential` with `--card` through the whole card sequence, split at
  states: fields 3600-4500 (mount, file creation at 4348) 531 s; 4500-5300
  (Game A at 5188) 181 s; 6300-7100 (Game B at 6928) 236 s; the load
  sequence 0-600 (the boot-time read of the card and its file at 17-18) 98 s
  and 3000-3500 (Load Game to "Load successful.") 101 s. All clean.
- `gc_cardtest` with `--differential` stops at the program's own exit with
  "finished: recompiled 1, interpreted 0" at identical pc and instruction
  count - with or without a card, so it is the harness meeting a program that
  ends, not the card.

**Left open.**

- The clock: the real-time clock reads zero, so every save's timestamp is a
  couple of minutes after 2000-01-01 and restarts each boot. Shrek 2 keys
  its slots by name and does not care; a game that picks "the most recent
  save" by time would see them all as the same age.
- The hub keeps `<id>.card` across a rebuild but not `<id>.card.bak`; one
  more name in `GameCubeBackend::is_persistence_file()`
  (`hub/src/console_gc.cpp`) would keep it. Not done here because that file
  is being edited for the cover trace.
- `--card` is not in the frontend's list of runner-only options
  (`gc_sdl.cpp`), so it only selects the headless runner alongside another
  runner option such as `--frames`; alone it is ignored by the window, which
  names its card from `--save-dir`.
- Transfers still complete in the instruction that starts them. Nothing seen
  wants them slower (the library's watchdogs are 100 ms and 2 s), but a game
  that polls for the busy bit would be the first to say otherwise.
- The frontend's OpenGL renderer was not exercised with a card under a real
  window (dummy video falls back to software); the card path does not touch
  the renderer.

## Save states (2026-09-14)

The whole machine to a file and back, so that execution continues exactly as
if it had never stopped. Two users, in this order: `--differential`, which
used to interpret Shrek 2's whole intro (8-20 minutes) before it reached
anything worth comparing, and players, who get ten slots in the overlay menu.
`runtimes/runtime-gc/include/gc_state.h` has the format; `src/gc_state.c` the code,
with each subsystem's private state saved by functions at the ends of
`gc_pad.c`, `gc_card.c`, `gc_ax.c` and `gc_audio.c`.

### The format

A 176-byte header, then sections, each a four-character tag, a version and a
64-bit length:

| Section | What | Bytes |
|---|---|---|
| header | `NCGCSTAT`, format version, the layout fingerprint, game id, program-image hash, disc hash, the build that wrote it, field and instruction count, payload size, stored size, compression, checksum | 176 |
| `MACH` | `GcMachine` whole - `GcCpu`, the I/O registers, GX (bp/cp/xf, TLUT, TEV registers, FIFO pointers), the EFB's scalars, VI/AI/DSP/DI/EXI/SI state - with every pointer and host setting zeroed | 560 848 |
| `MEM1`, `ARAM` | main and audio memory | 24 MB, 16 MB |
| `EFBC`, `EFBZ` | the framebuffer's colour and depth planes | 1.35 MB each |
| `PADS` | the four ports, field by field (a `GcPad` has padding) | 52 |
| `CARD` | whether a card is in; its image and the command it is in the middle of | 4 or 524 336 |
| `AX` | the microcode's mixing buffers and voice-list pointer | 5 808 |
| `AUDI` | the played-sound ring (the indices are in `MACH`) | 192 008 |
| `END` | | 0 |

`MACH` is written whole rather than field by field on purpose: a field
someone adds to `GcMachine` is then saved without anyone remembering to, and
the one thing that needs attention - a pointer or a host-only field - is
named in `scrub()`/`keep_host()` (a comment above `struct GcMachine` says
so). The layout fingerprint (sizes and offsets of everything written whole)
refuses a state from a build whose structures differ; the game id and the
program-image hash refuse another game or another executable; the disc hash
(game code, title, DOL and FST offsets, and the FST itself) refuses another
disc. Everything is checked - header, decompression, checksum, every section
and each subsystem's payload - before the first byte of the machine changes,
so a refused load leaves it running. Each refusal says which of these it was.

Compression is a small LZ77 written for this (nothing is vendored that
compresses): literal runs and matches of four or more bytes within 64 KB.
Shrek 2's states are 7.3 MB at field 300 (the logos), 8.3 MB at 1000 (the
title menu), 13.9 MB at 1400 (the storybook) and 19.1 MB at 2000
(gameplay), against 45.4 MB uncompressed. `--state-uncompressed` writes the
raw form. Times at field 2000 on a quiet machine (the 265K): save 126-140 ms and load
51-55 ms compressed; 61-66 ms and 26-28 ms raw. Under the load of five other
runs they roughly double. The frontend copies the machine between fields and
compresses and writes the copy on a thread of its own, so a save stalls the
game only for the copy.

### What is saved, and what is deliberately not

Everything `gc_machine_reset` touches and everything `GcMachine` points at
that is the machine's: memory, the framebuffer, the pads, the memory card,
the AX state and the sound ring. The lazy FPSCR note from #35 is saved as it
is (a state hash settles it, because the interpreter and recompiled code can
hold the same FPSCR differently). The card's *image* is in the state as well
as its in-flight command: a game keeps the card's directory in its own memory
once mounted, so resuming against the card as it is on disk now would be a
game that is wrong about its card. Loading therefore puts the image back and
writes it to the card's file; a state saved with no card (the headless
runner has none) ejects the frontend's, and a machine with no card takes the
state's as an image in memory with no file.

Not saved, because none of it changes what the guest computes:

- `cpu.next_event`: every entry to recompiled code clears it.
- The shortcut maps to main memory (`cpu.fast`): rebuilt from the BATs on the
  next access, which is what already happens whenever the GL path or a BAT
  write forgets them.
- The dispatcher's table and the OS hooks: the program boots before a state
  is loaded over it, and that is what installs the hooks its symbols name.
- The rasteriser's per-thread row memos and the compressed-texture memo,
  keyed by their whole input, and the TEV plan, rebuilt per triangle.
- Coverage, logs, the profile, branch history, the watch and stop settings,
  and the frontend. The raster helper threads need no quiescing: every job
  finishes before the triangle that started it returns, and states are only
  taken between fields.
- `card.dirty` and when the card last changed, in a *hash*: they decide when
  the file is written, which the guest cannot see.

### How exactness was proved

Two hashes: the frontend's `--audio-stats` hash (MEM1 and the GPRs and FPRs,
FNV-1a) and `gc_state_hash`, which hashes the whole payload as it would be
written (ARAM, both EFB planes, every register and device) with the host
fields out and FPSCR settled. The runner's `--state-hash` prints both after
carrying the machine to `field x CYCLES_PER_FIELD + 100 000` instructions
with the interpreter, so a recompiled run (which finishes its block) and an
interpreted one are hashed at the same count.

**Shrek 2, `--auto-input`, to field 2,200.** A straight run in each mode; a
run in each mode that saved at fields 300 (logos), 1000 (the title menu),
1400 (storybook pages after New Game) and 2000 (gameplay) and went on to
2,200; and from each of those eight states a resumed run to 2,200 in both
modes - rec from rec, int from int, int from rec and rec from int. Every run
had to end with the same state line (instruction count, pc, both hashes),
every frame dumped every 50 fields after K had to be byte-identical to the
straight run's, and the sound it dumped had to be exactly the tail of the
straight run's `--dump-audio`.

| K (what is on screen) | rec from rec | int from int | int from rec | rec from int | frames compared after K | sound compared |
|---|---|---|---|---|---|---|
| 300 (DreamWorks logo) | same | same | same | same | 38 | 4 057 376 bytes |
| 1000 (title menu, New Game) | same | same | same | same | 24 | 2 562 560 bytes |
| 1400 (storybook pages after New Game) | same | same | same | same | 16 | 1 708 384 bytes |
| 2000 (gameplay) | same | same | same | same | 4 | 427 104 bytes |

All sixteen end at instruction 17,837,920,000, pc 8009ad30, `--audio-stats`
hash 2cc729891ba16b28, full hash 9d3d56c955d30358 - which is also where both
straight runs end, recompiled and interpreted.

The two saving runs ended with the same hashes as the straight runs, so
saving changes nothing. A state loaded and saved again is byte-identical to
the original. The frontend loading a state (SDL dummy drivers, `--load-state`
with `--audio-stats`) and the runner loading it with an at-rest controller
(`--input` of `0 -`) reach the same `--audio-stats` hash at field 1,460.

**Without the disc:** `gc_state_roundtrip` (CTest) runs ten test programs
(`gc_spin`, `gc_drawtest`, `gc_audiotest`, `gc_cardtest`, `gc_padtest`,
`gc_disctest`, `gc_tevtest`, `gc_textest`, `gc_fogtest`, `gc_memtest`) on the
interpreter with a controller that changes every field and a memory card,
saves at a quarter, a half and three quarters of the way - inside fields -
and at the first field end, compressed and not, and holds every resumed run
to the straight one (instruction count, pc, field, full hash, and a hash of
all sound taken after the save point). It also checks that a state from
another program or game, a cut-short state, a damaged payload and a damaged
header are each refused with the machine untouched.
`gc_state_roundtrip_fresh_process` resumes the same states in a new process.
The menu self-test now also steps the slot with F7/F6, saves with F5, lets
the game run on and loads with F8, and requires the same field and the same
full hash back.

A run without states is unchanged: Shrek 2 to 3,700 fields with
`--auto-input`, frames every 100 fields (600, 1,200, 2,000, 2,500, 3,000 and
3,700 among them) and the whole `--dump-audio`, and a plain run's frame at
3,700, are byte-identical to main's build (f1e8a7b).

### Starting a differential from a state

    G3RE52 --disc DISC --frames 1400 --auto-input --save-state s1400.state
    G3RE52 --disc DISC --load-state s1400.state --frames 1500 --auto-input --differential

`--frames` stays absolute (the second command runs 100 fields), and
`--input`/`--auto-input` keep playing by field number. Both machines load the
same state, and if they diverge the block search starts from the state too.
`--save-at` takes a list (`--save-at 300,1000,1400`), putting the field in
each file's name.

From boot, `--differential --frames 1500` took 325 s; from the state at
1400, 19 s (the same machine, quiet, back to back).

### The frontend

Ten slots, `<id>.state1` to `.state10` beside the executable or in
`--save-dir`, listed in the hub's persistence files so an export or a rebuild
keeps them. The menu has a Savestates section like the GBA frontend's: active
slot, Save, Load, Delete, the slot's file and whether it is there. Shortcuts
follow GBA: F5 save, F8 load, F6/F7 previous/next slot on Windows and Linux;
Cmd+S, Cmd+L, Cmd+[ and Cmd+] on a Mac; on a Steam Deck the back paddles, L4
save, L5 load, R5 next slot (GBA's X and Y are GameCube buttons). The
selected slot is kept in `gc_prefs.ini`. Loading clears the sound queue and
restarts pacing, because what was queued belongs to the moment the game
left. `--load-state FILE` starts the window from a state.

### Left open

- Other platforms: the layout fingerprint includes pointer size and every
  structure's size, so a state from one compiler is refused by another; that
  is by design until someone needs to move states between them.
- Rebuilding a game with a runtime whose structures changed invalidates its
  states (refused with a message, never misread). A converter could carry
  them over; nothing needs one yet.
- Loading a state writes its memory card image back to the card's file, so
  saves made on the card after the state was taken are gone from it. That is
  the price of a game whose memory agrees with its card; the menu says so.
  The card it replaced is kept in `<card>.bak` (2026-09-15).

## The DSP, and the mail that has to arrive late

Shrek 2 boots, sets its memory up, resets the DSP, issues its first drawing
commands and copies a frame out - and then waits, for ever, on a callback
that only the sound processor can cause. That is what made P6 wait for P5:
a game does not treat sound as optional, because its frame loop is hung off
the audio one.

What the machine was doing was answering the DSP's start-up handshake, the
ROM's, and nothing else. What a game does is upload its own microcode - AX
- and talk to *that* instead. The two conversations use the same mailbox
and say different things, and the machine was replying to the second with
the first one's greeting.

Telling them apart turned out to need no guessing at all. The start-up
handshake boots a microcode at address zero; the game boots one at
`0x802f10e0`, 7872 bytes long. Printing both was the whole experiment:

```
=== a libogc program ===
gc: the DSP booted microcode at 00000000 (0 bytes)
=== the game ===
gc: the DSP booted microcode at 00000000 (0 bytes)
gc: the DSP booted microcode at 802f10e0 (7872 bytes)
```

So the machine answers on the address, not on a guess about the size or on
which mail arrived first.

After that the protocol is five messages, all of them from the microcode,
and two of them are enough to run a game: *the task has started* and *the
task is offering to stop*. The driver acknowledges the second with "carry
on" and hands over the next command list, and that loop is a game's audio
frame. `docs/gc-dsp-ax.md` has all five and where each comes from.

**Two things about it would each have produced a machine that looks
right and hangs.**

The first is that mail does not interrupt. On hardware the microcode
raises the interrupt as a separate act after placing the word, so a
mailbox that is perfectly correct - the right value, the right waiting
flag, cleared by the right read - still leaves the game asleep, because
nothing ever woke it. That is why the answer is placed by `gc_dsp_advance`
and the interrupt raised there too, rather than by the act of writing the
mailbox.

The second is that a reply must not be instant. Nothing in the machine is
doing any work, so every answer *could* come back in the same instruction
the command list was handed over in - and at least one commercial title
hangs when it does, because it keeps its own idea of how long a list takes
and does not believe a faster one. The delay here is a floor rather than a
measurement: far above the smallest known to avoid the hang, far below
anything a game would notice. **Unverified:** what the hardware really
takes, which depends on the list.

With those in place the same run gets 111 fields, 1062 triangles and 58
copies out before it stops - a game running its frame loop rather than a
game waiting. What the command lists actually *say* - the voices, the
mixing, the ADPCM - is still ahead, and is what turns this into sound.

## The recompiler

`recompilers/recompiler-gc/`, built as `gcrecomp`. It takes a `.dol` or an ELF and
writes a C project that *is* that program: one function per basic block,
one call per guest instruction, and a dispatcher that picks the block for
the current program counter. ELF is accepted deliberately - every program
in `tests/gc/programs` is one, and a gate that needed a commercial disc
image would be a gate nobody could run.

**The emitter sequences handlers and never implements one.** Each guest
instruction becomes a call to the same `gc_op_*` function the interpreter
calls, with the instruction word as a compile-time constant so the field
extractions inside it fold away. Everything around the instruction - the
hooks, the interrupts, the retirement, the video interface - is the
runtime's own `gc_machine_before` and `gc_machine_after`, called where
`gc_machine_step` calls them. And the map from opcode to handler is
`gc_cpu_ops.h`, the single list the interpreter's dispatch is also built
from. Three readers, one table: that is the whole defence against the
Game Boy failure of an emitter and an interpreter drifting apart.

**The analyzer follows flow; it does not sweep.** A linear sweep over a
text section decodes jump tables, string literals and alignment padding as
instructions. Following control flow from the entry point and the
exception vectors finds only code it can prove is reachable, and
everything it misses falls through to the interpreter rather than becoming
wrong. That asymmetry is the point: missing a block costs speed, inventing
one costs correctness.

**Interrupts are checked between every instruction, not between blocks.**
A block that ran several instructions between checks would deliver
interrupts late and move the beam in jumps, and `gc_timetest` measures
both. The speed in a recompiler is meant to come from not fetching and not
decoding - which is most of the per-instruction cost - and it does not
have to come from skipping this.

(Since 2026-09-13 recompiled code *looks* only at the instructions where
something can be due, which the machine works out in advance; every
interrupt still arrives before the same instruction. See "Faster, measured
on a game" below.)

### The gate

`tools/gc/run_recompiled_tests.py` recompiles each guest program, builds
it, and runs the result twice: once through the recompiled blocks and once
with `--interpret`, which is the *same executable* driving the *same
machine* through the interpreter. Comparing against a separate interpreter
would leave room for the two to differ by the harness; comparing an
executable against itself leaves room only for the thing under test. It
checks the verdict lines, the instruction count and the final program
counter, and every program agrees on all three.

It checks the programs side by side, and each one's two runs at the same
time, with the processor's threads shared between the compiles. Nothing it
compares depends on wall-clock time. With `--slow` on a 20-thread machine
(2026-09-13): **118 s**, against 504 s of stage time summed over the 11
programs, which is roughly what checking them one after another took. Each
program prints its stage times and the run ends with the totals; the wall
time is now set by the two long runs, `gc_spin` (87 s for both) and
`gc_timetest` (46 s), and then by CMake's configure step (9 s a project).

### How fast, and how that was found out

**1.43 times the interpreter on work that is actually the processor**:
`gc_timetest`, 2.6 billion instructions, 32.6 s recompiled against 46.6 s
interpreted. On `gc_drawtest` the two are level, because that program
spends most of its time in the rasteriser and recompiling the processor
does not make the rasteriser faster - which is worth stating plainly,
since a benchmark chosen badly here would have reported either number.

Getting there took three measurements and two wrong guesses, which is the
part worth writing down. The first version was *slower* than the
interpreter it replaced - 0.77 s against 0.44 s on `gc_drawtest` - while
demonstrably doing less work per instruction.

- **Guessed: code size.** The handlers are `always_inline`, so eleven
  thousand call sites expand eleven thousand copies of the arithmetic; the
  Game Boy work found per-instruction code size to be the lever there.
  Letting the compiler choose instead made it very slightly *worse*.
- **Guessed: the calls across the runtime boundary.** Every instruction
  calls `gc_machine_before` and `gc_machine_after` in another translation
  unit, where the interpreter has them inlined. Link-time optimisation
  made no difference either.
- **Measured: the dispatcher.** It ran between every block - which on real
  code is every few instructions - and halved a sorted range to find one,
  eleven dependent loads at a time. Replacing that with an array indexed
  by instruction number, one bounds check and one load, took 0.77 s to
  0.47 s in a single change.

The lesson is the ordinary one and it still had to be paid for: the
plausible cause and the actual cause were different, and only the third
thing measured was the one that mattered. `gc_cpu_impl.h` keeps the hook
that made the first experiment possible, because the next person to
suspect inlining should be able to test it in a minute rather than an
hour.

What is left is per-instruction overhead - two calls into another
translation unit and a check that the program counter still points where
the block thinks it does - and that is P8's.

### Faster, measured on a game (2026-09-13)

Shrek 2, recompiled, headless under SDL's dummy drivers, 1,800 fields from a
fresh memory card, no input:

| | fields a second | state at field 1800 |
|---|---|---|
| before | 6.7 | `85be6e5976c3d4a8` |
| display interrupt slots | 6.8 (profiled build, from 6.3) | same |
| work only when due | 18.2 | same |
| loads and stores straight to memory | 21.5 | same |
| rasteriser weights once a pixel | 25.2 | same |
| **all of it, regenerated from scratch** | **24.9** | **same** |
| the same executable with `--interpreter` | 5.8 | same |

**3.7 times** what it was, and 41 % of the console's rate. The last frame is
byte-identical in every row. The state number is the hash `--audio-stats`
now prints - main memory and the registers, after the interpreter has carried
the machine to the same instruction, 100,000 past the last field boundary,
because recompiled code finishes its block before it looks at the field
counter and so stops a few instructions later.

Two things about that number cost an afternoon each and are worth knowing
before using it. **The memory card's serial is seeded from its path**, so
two runs only agree if they use the same `--save-dir` - and a recompiled
executable names its card after itself where `gc_play` names it after the
disc, so those two never agree however right they are. And **a hash that
agrees at field 150 proves little**: the first real bug below did not show
until field 600.

**Every change was chosen by a sampling profile of the game, not a guess.**
The profiler is a thread that suspends the main one every millisecond and
records its instruction pointer, symbolised with DbgHelp at exit - the GBA
work's pattern, linked into a copy of the generated project built with
`/Zi`. The first profile, before any change:

| where | share of the run |
|---|---|
| between instructions: `advance_video` 22 %, the audio/DSP clocks, hooks, interrupt checks, retirement | about 64 % |
| of which `io_slot` and `strcmp`, from `gc_vi_update_interrupt` | 6.9 % |
| the bus: `bus_read`, `translate`, `gc_efb_read`, the byte loop | about 10 % |
| the rasteriser and texture sampling | about 22 % |
| **the game's own code** (`gc_block_*`) | **5.6 %** |

The earlier conclusion that "the machine itself has no remaining hot spot"
was true of a program doing nothing and not of a game: a game programs the
display interrupts, and every time the beam moved a pixel the interrupt line
was recomputed by finding four registers **by name** - the same mistake the
audio registers had, one function over. Fixed in twenty lines.

**The work between instructions is now done only when it is due.** The CPU
carries `next_event`, the earliest instruction count at which the machine
could have anything to do: the field ending, a display interrupt's target,
the decrementer running out, the audio clock reaching its target, DSP mail,
an audio block, an interrupt already waiting. Generated code compares the
count against it before each instruction and calls `gc_machine_due` only
when it is reached; anything that could move the moment - an I/O access
(which also brings the beam and clocks up to date first), `mtmsr`, `rfi`, a
DEC write, any interrupt line moving - clears it. The interpreter is
unchanged and remains the reference, so the recompiled-test gate compares
the scheduled machine against the per-instruction one on every program.
`gc.c` has the argument for why the two are observably the same.

That argument was wrong twice, and **neither mistake was visible to any
test program** - the gate passed and gc_timetest reported identical
instruction counts both times:

- *The beam brought up to date twice at one count.* At the instruction
  where a frame ends, `advance_video` holds the beam on the last pixel and
  moves to the next field; asked again at the same count, it wrapped the
  beam - raising the retrace one instruction early. A lock-step harness (two
  machines, one recompiled and one interpreted, registers compared after
  every block and memory every two million instructions) stopped on it at
  field 16. `advance_video` now does its work once per count.
- *A machine handed to the interpreter with something due.* A block can
  return exactly at the count where an interrupt becomes due; if no block
  starts at the next address, the dispatcher steps the interpreter, which
  checks for the interrupt *before* catching up and took it one instruction
  late. This only happened with a memory card inserted, took until field
  600 to change the state, and the lock-step harness could not see it for a
  reason of its own: the EXI device selection is file-static in `gc_io.c`,
  so two machines in one process share it. It was found by logging the
  recompiled run's state at every field and replaying the interpreter to the
  same counts in a second process. The dispatcher now catches the machine
  up before a fallback step and before it returns.

**Loads and stores go straight to main memory when the BATs say they can.**
With the per-instruction work gone the bus was a fifth of the run. The CPU
now holds a map of each 16 MB of the address space - one with data
translation off, one on - filled in only where the long way would certainly
land in main memory by the same arithmetic: not the locked cache, a single
BAT covering the whole piece and matching first, a physical address in
memory. Everything else still walks. The map is emptied by DBAT and HID2
writes and a reset, and kept empty while an address is watched or the
OpenGL path is running, because both of those see every store.

**The rasteriser's neighbour weights once a pixel.** With the processor no
longer the bottleneck, 17 % of the run was two lines computing texture
coordinate gradients: the barycentric and perspective weights at the pixel
to the right and below, recomputed for each of the eight coordinate sets.
They do not depend on the set; the expressions are unchanged, only hoisted.

**Tried and dropped.** Forcing the new load/store functions inline, after the
profile showed MSVC had left `gc_bus_fast_read32` as a call: 25.3 fields a
second against 25.2, and the generated project took 927 s to build instead
of 417. Reverted.

**gc_timetest does not measure the recompiler, and the 1.43x above does not
reproduce.** Measured again before any of this, it took 44.1 s recompiled
and 43.4 s interpreted; after, 44.2 s and 41.7 s. A profile of the
recompiled build puts 0.15 % of the time in generated blocks and a quarter in
`gc_cpu_step`: the program spends its time in libogc's idle thread, a single
`b .` at `0x800050bc` that is reached only by a context switch, so the
analyzer never finds it and every iteration is the interpreter fallback. It
still does its real job - the instruction count at the end is identical
before and after, which it would not be if one interrupt moved - but it is
not a speed benchmark for recompiled code.

**Where the time goes now** (profiled build, 23.8 fields a second): the
rasteriser's triangle loop 23 %, texture decoding and sampling (CMPR
`decode_compressed` 8 %, `sample_level` 8 %, palettes, `level_of`) about
25 %, `gc_gx_write_pixel` 5 %, the EFB copy 3 %; the game's own code 14 %;
the remaining loads (mostly the load itself) about 11 %; the dispatcher 1.6 %.
**The next lever is the texture path, not the recompiler**: every texel of
every sample is decoded from guest memory through `gc_machine_ram`, and a
compressed texture is re-expanded four times per bilinear sample. After that,
the per-pixel loops over all eight coordinate sets when a stage uses one or
two. On the processor side, direct block-to-block chaining would save the
dispatcher's share, and skipping ahead to `next_event` on a `b .` would make
idle loops like gc_timetest's cost nothing.

### How long a game takes to build (2026-09-13)

A hub build of Shrek 2 took **12 minutes 41 seconds**, of which 12 m 37 s
was compiling, and all of that was waiting on one file. `gcrecomp` cut a
`recompiled_blocks_NNN.c` every 2,048 blocks regardless of what was in
them; the ninja log of that build (Intel Core Ultra 7 265K, 8 + 12 cores,
ninja's default 22 jobs, another session compiling at the same time):

| edge | seconds |
|---|---|
| `recompiled_blocks_006.c` (one block of 393 instructions) | about 755 (last to finish; the log copy missed its line) |
| `recompiled_blocks_000.c` | 442 |
| `recompiled_blocks_005.c` | 328 |
| `recompiled_blocks_004.c` | 253 |
| `recompiled_blocks_010.c` | 86 |
| `runtimes/runtime-gc/src/gc_cpu.c` | 45 |
| the other seven block files | 4 - 33 each |
| everything, recorded | 1,327 plus 006 |

Timings on this machine move by a factor of two with what else is running
(an earlier hub build of the same game took 7 m 24 s), so every comparison
below was run back to back.

**What makes a file slow is the length of its functions, and which
instructions are in them.** Every `GC_INSN` inlines its handler, and MSVC's
optimiser time grows much faster than the function. A microbenchmark - 480
copies of one handler, as 480 functions of one instruction or fewer longer
ones, `cl /O2 /fp:strict`, header cost subtracted, several compiles at a
time (so read the rows as ratios):

| handler | 1 per function | 4 | 16 | 48 |
|---|---|---|---|---|
| `ps_add` | 5.0 s | 10.6 s | 35.7 s | - |
| `psq_st` | - | 0.6 s | 3.1 s | 12.5 s |
| `addi` | - | ~0 | ~0 | ~0 |

The floating point arithmetic (paired-single and scalar add, subtract,
multiply, divide, the fused forms, compares, conversions) and the quantised
stores carry the whole IEEE flag model into every copy; a load, a branch or
an integer operation costs almost nothing.

**Three changes, each measured on the whole game** (81 files unless said):

| | build wall | block files' work | longest edge |
|---|---|---|---|
| main: 12 files by count, blocks whole | 12 m 37 s | ~2,000 s | 006, ~755 s |
| parts of at most 48 instructions | 104 s | 1,549 s | a block file, 84 s |
| parts of 16 | 57 s | 862 s | 42 s |
| parts of 8 | 40 s | 583 s | `gc_cpu.c`, 35 s |
| parts of 4 | 34 s | 489 s | `gc_cpu.c`, 29 s |
| parts of 2 | 30 s | 457 s | `gc_cpu.c`, 22 s |
| parts of 4, files by weighted cost: 81 / 41 / 24 / 16 files | 32 / 35 / 33 / 32 s | 488 / 437 / 405 / 301 s | `gc_cpu.c`, 27 - 30 s |
| **and the interpreter's handlers out of line: 21 files (the default)** | **30 s** | 384 s | a block file, 25 s |

1. *A long block is a chain of functions* of at most four instructions
   (`--part-insns`). Each part ends by calling the next as its last
   statement, and a part is never inlined back (`GC_PART`). A `return`
   anywhere leaves the whole chain exactly as it left one function, the
   per-instruction event check is unchanged, and the dispatcher sees the
   same block boundaries.
2. *Files are cut by estimated cost*, not by count: an instruction costs 1,
   the heavy floating point handlers 8, a function 1, about 10,000 to a
   file (`--files` overrides). Fewer, bigger files cost less work in total -
   each file pays a second or two for the runtime's inline headers - until
   the biggest file becomes the wait.
3. *The interpreter calls each handler as a function of its own.* Inlined
   into one switch, the 221 handlers made `gc_cpu.c` one function that took
   10.3 s alone and 30 s beside a game's build, the last edge every build
   waited for; as 221 small functions it is 1.7 s. The interpreter did not
   get slower: Shrek 2 through `gc_run` for 240 fields, two rounds each,
   5.68 and 5.61 fields a second before, 5.82 and 5.84 after, the same
   instruction count and program counter.

Also: the generated project no longer builds `gc_run` and `gc_play`
(`GC_RUNTIME_TOOLS` off, and the runtime added `EXCLUDE_FROM_ALL`).

**The machine is unchanged, and so is its speed.** Shrek 2 recompiled, 1,800
fields through the SDL frontend under dummy drivers, back to back with one
save directory:

| build | fields a second | state at field 1800 |
|---|---|---|
| main (the hub's build) | 24.6, 24.3, 24.2 | `fbf32add72006565`, 14,594,680,000 instructions, pc 8005b57c |
| parts of 48 / 8 / 4 / 2, 81 files | 24.6 / 23.8 / 24.7 / 24.6 | same |
| the default (parts of 4, 21 files) | 24.2, 24.2 | same |

and the default build in round one's save directory gives round one's
reference, `35f3599d8702e078`. The game's own code is 14 % of the run, so a
call every four instructions inside long blocks does not show; the
executable grows from 33.2 MB to 34.8 MB.

Now the build is limited by the processor rather than by any one file:
428 s of work on twenty cores in 30 s of wall time.

### Faster again: the pixel path (2026-09-13, round two)

Same run, 1,800 fields, three interleaved runs each. The machine was at
up to 96 % load from other work, so wall-clock time drifted by as much as
20 % between runs; the table uses **process CPU time**, which moved by
about 3 %. Every row has state hash, instruction count
(14,594,680,000) and program counter (`8005b57c`) identical to main. The
hash differs from the round-one table's number only because the save
directory is a different path, and the card's serial is seeded from it.
Frames every 100 fields up to 3,700 are byte-identical to main under
`gc_run --dump-frame-every 100`. That covers the intro video, the
storybook title and its render-to-texture page.

| | fields a CPU-second | wall |
|---|---|---|
| main | 24.8, 23.7, 25.0 | 24.7 |
| registers once a triangle, sample points once | 32.3, 31.8 | 31.5 |
| **and compressed blocks remembered by their bytes** | **33.2, 34.2, 34.2** | **34.0** (57 %) |

**Registers once a triangle.** A triangle cannot write a register or main
memory while it is drawn. So `gc_gx_tev_prepare` reads every stage's
environment, scale and texture binding once per triangle, including all 17
mip levels' addresses and sizes, which `level_of` had re-derived per
sample. `gc_gx_tev_run_planned` then only does the arithmetic.

**Sample points once.** The gradient for a pixel needs the points to the
right and below, and those are the next pixels' own points. The rasteriser
now keeps the current and next row of points, so each is computed once
instead of up to three times. This is exact because
`(px + 7/12) + 1.0 == (px + 1) + 7/12` for every px from 0 to 4095, checked
exhaustively. It is not exact for some negative px, which the clamped
scissor box never produces. Only the coordinate sets some stage or its
indirect lookup reads are interpolated.

**Texel reads** test main memory inline instead of calling
`gc_machine_ram` in another file. `floorf` and `fabsf`, which strict
floating point leaves as library calls, are replaced by bit-exact
equivalents.

**Compressed blocks.** A block's four colours depend only on its two
endpoint words, so the last block's colours are kept, keyed on those words.
The words are read from memory on every texel, so the cache has nothing to
invalidate.

**Tried and dropped:**

- *A decoded-tile cache,* keyed on level address, size, format and palette
  binding. Epochs began and ended around each `gc_gx_run`, and ended after
  every EFB copy, lookup-table load and watch hook. The profile showed
  decoding gone, yet interleaved runs came out 1 % *slower* (30.7-31.0
  against 31.2-31.4). The gather pipe runs the command stream every 32
  bytes, so an epoch is roughly one draw. Each epoch re-decoded whole 64-texel
  tiles to serve a handful of bilinear taps. A cache that does not win is
  not worth its invalidation argument, so it is not in the tree.
- *Luma and chroma once a pixel in the display copy,* and the clear colour
  quantised once: no measurable change.
- *Skipping the per-stage pixel copy in the combiner, and a shortcut for four
  equal bilinear taps:* both exact, both within noise (32.9-33.8 against
  32.6-33.5).

**Where the time goes now** (profiled build): the game's own code 19 %;
`gc_bus_fast_read32` 11 %, most of it the load itself (`gc_bus.h:63`);
sampling (`sample_level`, `decode_compressed`, `bound_texel`) about 19 %;
the combiner 11 %; the triangle loop and `finish_point` 12 %;
`gc_gx_write_pixel` 5 %; the EFB copy and display conversion about 8 %;
the dispatcher 2 %. No single line in the pixel path is above 6 %. The
largest is the bilinear blend's float arithmetic, which has to stay as it
is. **The next levers are on the processor side:** chaining blocks
directly, and skipping ahead to `next_event` on a `b .` idle loop. Then a
real hit-rate measurement before any texture cache is tried again, for
example a per-draw decode of only the texels a bilinear footprint touches.

### Faster a third time: wait loops, and the pixel path again (2026-09-13, round three)

Same run: Shrek 2, 1,800 fields through the SDL frontend under dummy
drivers. The machine was shared with other work at 25-95 % load, so every
comparison was made two ways: **A/B**, both executables at once, each pinned
to its own pair of performance cores, rounds alternated, CPU time per
process (`ab.py` in the round's scratch tools); and back to back unpinned.
Pinned numbers are lower than unpinned ones on the same build, so compare
within a column. Every kept row has instruction count 14,594,680,000, program
counter `8005b57c` and the same state hash as main in the same save directory,
and fields 600, 1,200 and 3,700 byte-identical to main's under
`--dump-frame`.

| | A/B, fields a CPU-second | step |
|---|---|---|
| main | 27.7 | |
| wait loops counted rather than run | | +38 % unpinned (32.9 to 45.5) |
| bilinear blend in SSE2, compressed colours in 64 slots | | +2.9 % |
| whole-program optimisation for the pixel path's files | | +9.2 % |
| YCbCr and gamma tables, one clear colour narrowed once | | +1.8 % |
| a compressed footprint reads each block once | | +5.6 % |
| combiner helpers inlined | | +0.9 % |
| framebuffer conversions inlined | | +2.2 % |
| **all of it** | **51.1** | **1.84 times main** |

Unpinned on a quiet machine, back to back: main 33.9 fields a CPU-second,
the build before the last row 60.0 (and 59.7 a second of wall time, which is
the console's rate - the frontend paces to it). The generated project takes
about 3 s longer to build (29-31 s against 32-33 s, twice each back to back),
which is the link-time code generation.

**The profile said the obvious guess was wrong a third time.** The brief was
the processor side: chaining blocks, the load path, the per-instruction
prologue. A profiler that also records each sample's caller and names
generated blocks individually put **28 % of the whole run in one block of
three instructions** - `lwz r0, -0x7848(r13); cmplwi r0, 0; beq` at
`802474c0`, a game waiting for an interrupt handler to set a flag - with the
load (`gc_bus_fast_read32`, 12 %) charged to it. Every other generated block
together was under 5 %.

**Wait loops are counted rather than run.** The emitter recognises a block
that branches back to its own first instruction and whose second iteration
provably repeats the first: every instruction is a load, a compare, register
arithmetic without carry, or the branch back, which neither links nor
counts; no store; and nothing the loop reads before writing it is written by
the loop (general registers, condition register fields, XER). Generated code
runs the loop once as always; if that iteration went back to the start, every
load's address was in the shortcut to main memory, and no instruction reached
`next_event`, the whole iterations that fit before `next_event` are added to
the count at once. The instructions that reach it run one at a time, so an
interrupt arrives before the same instruction as before. `gc.c` has the
argument, next to `gc_machine_due`. The emitter finds 44 such loops in
Shrek 2, several of them polling a device register - which is never in the
shortcut, so those always run - and 10 to 25 in each test program.

**Then the pixel path again**, because with the waiting gone the processor
side was 10-12 % of the run in total and the rasteriser was 75 %:

- *Whole-program optimisation (`/GL`) on the framebuffer, command stream,
  rasteriser, combiner, sampler, fog and blend files.* The pixel path is a
  chain of small functions across those files, called once a pixel, and
  MSVC inlines across files only this way. 9 %. The recompiled blocks are
  left out - link-time code generation over them would cost the build - and
  so is the rest of the runtime, which measured nothing (45.6 against 45.6).
  Other compilers are unchanged.
- *The bilinear blend* does four channels side by side with the same
  single-precision operations in the same order, checked equal to the scalar
  form over 400 million inputs including NaNs, infinities and random bit
  patterns. The clamp agrees on a NaN because `maxps` returns its second
  operand, zero, and the scalar conversion of a NaN is zero in its low byte.
- *A compressed footprint* wraps each axis once, groups the four texels by
  block address, and decodes each distinct block once (copied out of the
  colour memo, because the next block's lookup can reuse the slot). The same
  arithmetic, grouped. 5.6 %.
- *Tables instead of products*: each product in RGB to YCbCr and back is a
  table of doubles filled by the same expression and summed in the same
  order, so the results are the same bits; the gamma curve is a table of what
  `pow` gave; a clearing copy narrows its one colour once.
- *Forced inlining* of the combiner's and the framebuffer's per-channel
  helpers, which MSVC left as calls even under `/GL`.

**Tried and dropped:**

- *Direct block chaining.* After the wait loops the dispatcher is 0.8 % of
  the run (it was 2.3 % before them, most of it from that one loop), so the
  most it could save is below the noise. And C does not guarantee a tail
  call: a chain of blocks that call each other grows the stack until
  something returns, which a loop never does, so it would need a trampoline -
  which is the dispatcher. The run loop in `recompiled_run.c` is still the one
  place every block returns to and where a field end is counted.
- *Per-instruction prologue and load path.* The game's own code is 4.3 %, the
  out-of-line loads and stores 3.6 %, and the slow bus (store to the gather
  pipe, translation) 2.2 %. Nothing there is large enough to measure a change
  against, and forcing the loads inline doubled the build in round one.
- *Colour and texture-coordinate interpolation in SSE2 pairs* (exact, same
  products and sums): 49.08 against 49.00, four rounds each. Reverted.
- *Forced inlining of the blend and sampler helpers*: 51.00 against 50.95.
  Reverted.
- *Finding each row's covered span instead of scanning the bounding box.*
  Counted first: the first 1,800 fields draw only 17,942 triangles, visiting
  554 M pixels of which 276 M are covered, so the uncovered half costs only
  its edge functions - about 1.5 % by estimate. Not tried.
- *Rows of a large triangle on worker threads.* The combiner's registers
  carry values from one pixel to the next, so pixels of one triangle are not
  independent.

**Where the time goes now** (profiled build, just before the last row): the
triangle loop with the blend inlined into it 21 %; `sample_level` and the
footprint 13 %; the combiner 11 %; `finish_point` and `row_point` 11 %; the
level of detail and mip selection 8 %; the command stream and vertex path
7 %; framebuffer quantising, the display copy and the frame for the window
about 7 %; the game's own code 4 %; loads, stores and the slow bus 6 %; the
dispatcher 0.8 %. About 276 M covered pixels in 1,800 fields is about
150 k a field - most of them full-screen quads in the intro video and the
storybook - at roughly 600 cycles each. **The next lever is per pixel**: the
four divisions in `finish_point` for every point, the combiner's generic
per-channel selection, and the level of detail recomputed for every pixel of
a quad whose gradient does not change. Gameplay, rather than this intro,
should be profiled before choosing, since it will draw many more, smaller
triangles and run more of the game's own code.

### Full speed in a window: the P5 run (2026-09-13, round four)

The goal was P5's last gate item - a 30-second windowed run with no audio
underruns - and holding the console's rate through the storybook title.
Main (`c1e7da6`) managed 55.8 fields a second over 30 s (262 underruns) and
32.8 over 90 s (776).

**Where the time went, before changing anything.** An instrumented copy of
the generated project (never the tree) logged, for every field, the wall and
thread-cycle time of running the machine, of presenting and of pacing, with
triangles, pixels and EFB copies; unpaced, SDL dummy drivers unless marked:

| fields | scene | run ms | present ms | fields a second | triangles | covered px | EFB copies |
|---|---|---|---|---|---|---|---|
| 0-1,700 | intro video | 15.1 | 0.71 | 63.2 | 10 | 152 k | 0.5 |
| 0-1,700, real window | | 15.6 | 1.05 | 60.2 | | | |
| 1,700-2,400 | legal screen | 14.7 | 0.75 | 64.7 | 10 | 155 k | 0.5 |
| 2,400-2,700 | loading, screen still | **94-107** | 0.86 | **10** | **0** | 6 k | 0.3 |
| 2,700-3,000 | storybook title opens | 75-79 | 0.72 | 13 | 433 | 978 k | 2.8 |
| 3,000-3,700 | storybook title | 83-116 | 0.76 | 9.7 | 480 | 1.43 M | 3.0 |

So the window was not the problem - it adds 0.35 ms of present a field - and
there were two slow sections, not one. **Fields 2,400-2,700 draw no
triangles at all**: the profile was 22 % `fesetround`, 16 %
`fesetexceptflag`, 16 % `fegetround`, and most of the rest the game's
paired-single decoder, running in non-IEEE mode. **The storybook** was the
pixel path: the triangle loop 28 %, the combiner 16 %, `sample_level` 14 %,
`finish_point` 13 %, sampling 10 %, EFB copies 3 %. Its 480 triangles a
field visit 3.2 M pixels and cover 1.43 M: a third depth-tested early, 62 %
late, 42 % of covered pixels failing the alpha test and none failing a late
depth test. One combiner stage, 81 % of pixels textured, and no stage reading
a register another writes.

**Kept, each with state at 1,800 and 3,700 fields (instruction count,
program counter, hash), frames 600, 1,200, 2,000, 2,500, 3,000 and 3,700
byte-identical to main, GC CTest, every recompiled test program, and a
600-field `--differential --auto-input`:**

| change | measured |
|---|---|
| host floating-point state through MXCSR rather than the C library; `isinf` as a bit test; `ldexp(1, k)` as built bits; non-IEEE mode's recomputation toward zero skipped for results at least twice the smallest normal | loading 94-107 to 62 ms a field |
| **rows of a triangle on helper threads**, and each row starting at its covered span | intro 15.2 to 5.6 ms; storybook about 100 to 27 ms |
| EFB copy rows on the helpers; triangles of 2 rows and 256 px of bounding box split (was 8 and 4,096) | storybook 28.7 to 16.0 ms; intro 5.5 to 4.0 |
| single-precision add, subtract, multiply and multiply-add without the host's flags where they are known | paired add 47 to 23 TSC ticks; loading 73 to 50 ms |
| the window's frame conversion on the helpers | present 0.8 to 0.25 ms |
| the full floating-point paths shared rather than inlined everywhere | generated build 37 to 22 s; loading 49 to 46 ms |

- **Rows on threads** (`gc_gx_raster.c`, `gc_gx_workers.c`). A pixel reads
  and writes colour and depth only at its own position; everything else it
  reads is fixed while a triangle is drawn; and the one thing it leaves for
  the next pixel, the combiner's colour registers, is split only when
  `gc_gx_tev_plan_independent` shows no stage reads a register group some
  stage writes before that pixel has written it. Each thread computes in a
  copy, and the machine takes the copy of whichever thread drew the highest
  chunk that ran the combiner. Indirect stages stay on one thread. The
  compressed-colour memo is per thread; helpers copy the caller's rounding
  mode. `GC_RASTER_THREADS` sets the helper count (default one fewer than the
  processors, at most 15; 0 turns it off) and `GC_RASTER_SPIN_MS` how long an
  idle helper spins before sleeping (4 ms).
- **Spans.** An edge function is monotonic along a row - every step of it
  is a rounding of a monotonic function of px - so the covered pixels of a
  row are one run, found by binary search asking the pixel loop's own
  question. Alone it measured the same as before.
- **EFB copies** split when no two rows can write the same byte: a display
  row fits its pitch, a texture row stays inside its row of tiles. A
  rectangle wider than its stride, or running off the framebuffer, stays on
  one thread.
- **Floating point without the flags** (`gc_fpu_impl.h`). In round to
  nearest (guest and host), with operands zero or between 2^-255 and 2^256,
  frA short enough that a double product is exact, and a single result that
  neither overflows nor nears underflow, the only flag is inexact, and it is
  TwoSum's error being non-zero or the narrowing to single changing the
  value. `tests/gc/gc_fpu_fast_test.c` holds that path to the full one over
  3 million boundary and random cases in every rounding mode (1.36 million
  answered fast, no differences); five of six deliberate mutations of the
  fast path fail it.
- **A race the gates caught.** A helper held up after seeing a job announced
  could, once that job and a later one had finished, run whatever job was
  current: one `--frames 3700` run of six sharing the machine crashed. A
  helper now runs a job only if it is still the announced one and still open,
  checked after counting itself in. Ten runs to 3,700 fields at once then all
  gave main's frame.

**Tried and dropped:**

- *The depth test before the combiner* for late-depth pixels: no storybook
  pixel failed a late depth test; the pixels thrown away fail the alpha test,
  which needs the combiner's alpha.
- *The first two of `finish_point`'s divisions as one packed division*:
  0.975 of the unchanged build, A/B pinned.
- *A 1 ms spin* before a helper sleeps: 0.3-0.5 ms a field slower in the
  intro and 4 % in the loading screen, from waking helpers by system call.
- *7 helpers instead of 15*: the storybook at 25 ms instead of 18. *Smaller
  chunks, or split thresholds below 2 rows and 256 pixels*: no change.
- **A measurement trap worth remembering**: the first threaded measurements
  showed almost no scaling from 3 to 15 helpers, because the instrumented
  build counted visited and covered pixels in two global counters every
  thread incremented. The plain build scaled; per-pixel shared counters in a
  threaded loop measure themselves.

**The P5 runs** (real window, WASAPI, `--mute --audio-stats`, save directory
and prefs in scratch, quiet machine, 2026-09-13):

| build | run | fields | fields a second | queue mean | ran dry |
|---|---|---|---|---|---|
| main `c1e7da6` | 30 s | 1,673 | 55.8 (93 %) | 14.5 ms | 262 |
| main `c1e7da6` | 90 s | 2,949 | 32.8 (55 %) | 12.6 ms | 776 |
| this round | 30 s | 1,798 | **59.9 (100 %)** | 61.1 ms, min 39.9 | **0** |
| this round | 90 s | 4,752 | 52.8 (88 %) | 44.5 ms | 589 |

**The 30-second gate is met.** The 90-second run is 643 fields (10.7 s)
short of the console's rate, and it loses them in two places.

**Where the time goes now** (headless, unpaced, by phase):

| fields | scene | run ms | of which graphics | game code | present ms |
|---|---|---|---|---|---|
| 0-1,700 | intro video | 3.8 | 1.1 (split raster 0.9, copies 0.15) | 2.7 | 0.25 |
| 2,400-2,700 | loading | 46-50 | 0.3 | **46-49** | 1.0 |
| 3,100-3,700 | storybook title | 14.8 | 9.7 (split raster 8.7, copies 0.3) | 5.1 | 0.25 |
| 4,700-6,000 | title, idle | 44-47 | 4 | **40**, interpreted | 0.9 |

- **The loading screen is the game's own paired-single code.** Between the
  floating-point instructions, about a tenth is loads and stores through the
  slow bus (`translate`, `page_table_lookup`: Shrek 2's virtual memory pages
  are not in the fast map) and the rest the per-instruction glue. 300 fields
  at 27 a second in the window cost about 6 s of the 90.
- **From about field 4,650 the idle title runs code the recompiler did not
  find**: `gc_cpu_step` and `advance_video` lead the profile. A build with a
  6,000-field `--trace` makes it native - but it also roots 0x802474c4 and
  0x802474c8, the middle of the wait loop at 0x802474c0 that interrupts
  return into, which splits the loop and loses the round-three skip: the
  intro's game code costs 21.6 ms a field in that build against 2.7 ms. Hub
  builds use the cover run's trace, so they have this too.
- **The storybook** holds 66 fields a second headless and 65 in the window:
  the split raster (whose last 12 % is the caller waiting for helpers to
  finish their chunks) and 5 ms of game code.

**The next levers**: keep a wait loop recognised when a trace roots its
interior (worth more to hub builds than anything else here); the slow bus
for virtual-memory pages (a translation cache would need invalidating on
page-table writes that bypass the bus); and the per-instruction glue in
floating-point-heavy blocks.

### Wait loops a trace had cut into pieces (2026-09-13)

The first of the levers above, measured on main before round four
(`c1e7da6`). The hub's cover run writes a trace and every rebuild passes it to `gcrecomp`
(see "Profiling" under P6), and a game rebuilt that way was *slower* than
its first build. Measured headless on an instrumented build: the intro video
cost 2.7 ms of game code a field without the trace and 21.6 ms with it; the
storybook title 5 ms against 24 ms.

**The cause was the wait loop at `802474c0`.** An interrupt returns to where
the program was, which is inside the loop, so the dispatcher entered the
interpreter at `802474c4` and `802474c8` and the trace recorded both. As
roots they cut the three-instruction loop into three blocks of one
instruction, none of which branches back to its own start, so
`idle_loop_shape` refused every one and the 28 % of the run that the skip had
removed came back. A branch from outside into a loop does the same without
any trace: Shrek 2 had six such loops the untraced build already missed.

**Fix:** the emitter (`idle_loop_end`) reads on through the blocks that
follow without a gap, up to the first branch, and when those instructions
form a wait loop it emits the block at the head as the whole loop, with the
same code the loop has when it is one block. The blocks inside stay, for a
run that enters there, and reach the head through the dispatcher. The table
entry for the head covers the whole loop, so the hook check still sees every
address it runs. Blocks that share addresses are harmless because the
dispatcher finds a block by where it starts. Shrek 2 now has 50 wait loops
with or without the trace (44 and 42 before), 6 and 8 of them across blocks.
`tests/gc/gc_split_wait_loop.py` (CTest `gc_split_wait_loop`) builds a
five-instruction DOL and checks the loop is emitted whole both ways; the
gcrecomp before this fails it.

**Same machine.** Shrek 2, the untraced build before (u0) and after (un) and
the traced build before (tb) and after (tn), all with a 6,000-field cover
trace from u0:

- Headless `--dump-frame` at fields 600, 1,200 and 3,700: all twelve frames
  byte-identical. Instruction counts and program counters identical for u0,
  un and tn (4,864,860,001 at 600 fields); tb stops one instruction earlier
  at 600 and 1,200 (at `802474c4`, having entered the split loop), the
  usual difference in where a block ends and not a change in the machine.
- SDL frontend, dummy drivers, 1,800 fields, each run given a copy of the
  same memory card: u0 twice, un and tn all `14,594,680,000` instructions,
  pc `8005b59c`, hash `dd667ec60a1cb6f1`. A fresh card per run gives a
  different hash on every run, so a card has to be copied in to compare.
- `--differential --auto-input` for 1,500 fields, un and tn: clean.
- `run_recompiled_tests.py --slow`: all 11 programs agree (none of them has
  a loop across blocks, which is why the new test exists). GC and hub CTest:
  14 of 14 pass.

**Speed**, headless runner, 3,000 fields, the three builds back to back for
three rounds in alternating order, one process at a time. The machine was
also running a game on about three cores, so CPU time is the figure to trust:

| build | CPU s (median) | wall s (median) | fields a CPU-second |
|---|---|---|---|
| traced, before | 157.0 | 159.0 | 19.1 |
| traced, after | 107.4 | 109.3 | 27.9 |
| untraced, after | 102.7 | 104.1 | 29.2 |

A traced build is 1.46 times faster than before. It is still 4.6 % slower
than the untraced build. Not investigated; the likely cause is the
roughly 12,000 extra blocks the trace adds, which cut hot code into shorter
pieces and so send more trips through the dispatcher. (It was not the
block count: see the next section.)

### The 90-second run on traced builds (2026-09-14)

Main `4fa86e3`, Shrek 2, three builds: untraced (u); traced with the hub's
cover run, 3,000 fields and no input, recorded from u (tc); traced with
6,000 fields of `--auto-input` (tl). Headless figures are the SDL frontend
under dummy drivers, unpaced, with a per-field log in an instrumented copy of
the generated project (run plus present, so fields a second as the machine
could go). Every run was started only when nothing else was running and CPU
load was under 30 %, and each logged the load of every other process once a
second; the tables below are runs where that stayed at 8-15 % throughout.
Several earlier runs overlapped another agent's benchmarks at 50-75 % and
read up to six times slower in the storybook: with 15 raster helpers, load
from elsewhere is not noise, it is a different measurement.

**Where each build was below 59.94, headless, fields a second:**

| fields | scene | u | tc | tl | uf | tf (tc's trace) | tcf (tc's old trace) | tlf |
|---|---|---|---|---|---|---|---|---|
| 0-2,400 | intro, legal | 216-329 | **188-285** | **189-281** | 213-331 | 235-377 | 234-366 | 224-372 |
| 2,400-2,700 | loading | **28.7** | **24.0** | **24.2** | **28.4** | **28.9** | **28.7** | **28.8** |
| 2,700-3,000 | storybook opens | 80.9 | 82.4 | 82.5 | 80.8 | 83.8 | 86.6 | 84.3 |
| 3,000-4,500 | storybook title | 67-78 | 71-83 | 70-82 | 67-79 | 70-82 | 71-83 | 70-82 |
| 4,500-4,800 | title | **37.8** | 82.6 | 83.0 | **37.5** | 84.0 | 85.0 | 82.5 |
| 4,800-6,000 | title, idle | **30-31** | 110-112 | 110-112 | **26-31** | 113-115 | 114-116 | 113-115 |
| 0-6,000 | | 65.5 | 98.1 | 98.0 | 64.9 | 106.3 | 107.4 | 106.1 |

(uf, tf, tcf and tlf are this section's builds: tcf and tlf take main's
cover and auto traces unchanged, tf a cover trace recorded by uf.)

**The windowed P5 run** (real window, WASAPI, `--mute --audio-stats
--limit-seconds 90`, a copy of the same card in each scratch save directory,
other load 8-9 %):

| build | fields | fields a second | queue mean | ran dry |
|---|---|---|---|---|
| main u | 4,751 / 4,745 | 52.7 / 52.7 | 35-36 ms | 597 / 610 |
| main tc | 4,803 | 53.4 | 45.1 ms | 507 |
| main tl | 4,817 | 53.5 | 36.8 ms | 493 |
| uf | 4,829 | 53.7 | 79.7 ms | 377 |
| tf | 5,043 | **56.0** | 83.2 ms | 296 |
| tlf | 5,045 | **56.0** | 85.8 ms | 295 |
| tf (earlier build, same code but the pool fix), twice | 5,049 / 5,055 | 56.1 / 56.2 | 60-80 ms | 298 / 294 |

**What the profile showed, and the three changes.**

1. **Traces were cutting blocks at interrupt returns.** An rfi comes back to
   the instruction it interrupted. In recompiled code that is the middle of
   a block, which has no table entry, so the dispatcher interprets from
   there to the next block start - and the trace records the address as an
   entry. Main's cover trace had 475 such addresses inside blocks static
   analysis finds on its own (of 5,877), the auto trace 2,084; as roots each
   cut a hot block in two. Worse, in code only the interpreter had run,
   nearly every instruction of a hot loop is interrupted at some point: an
   idle 6,000-field trace names 96 of the 119 instructions of the
   decompression loops at `800b7330`-`800b7508` (1.4 billion of the 1.62
   billion interpreted instructions), so a trace long enough to reach the
   title would have compiled those loops as blocks of one instruction. That is
   where main's traced builds lost 12-15 % in the intro and 16 % on the
   loading screen against the untraced build.
   - `gc_coverage_note` now marks an entry at SRR0 with MSR matching SRR1 in
     the bits rfi restores as *resumed*, and `--trace-out` leaves addresses
     that were only ever resumed at out (483 of them in the cover run; a
     branch that happens to match costs a root, never a wrong answer).
     `--coverage`'s most-interpreted list no longer splits regions there,
     which is what made it readable: nine of the untraced idle run's
     twelve entries were one instruction long before.
   - `gcrecomp` leaves out traced addresses strictly inside a block that
     static analysis finds without any trace, so traces already in a hub's
     `profile/` stop cutting blocks too (tcf above: 34,212 blocks against
     34,683, and the speed of a fresh trace). A new cover trace has 4 such
     addresses left.
   - Resumes inside code only a trace found are not covered by the second
     rule, which is why the first matters; the wait loop across blocks
     (#31) still has work in the six loops a branch enters in the middle.
     `gc_split_wait_loop` now builds that case (the old trace case expects
     the addresses left out and the loop one block), and `gc_coverage`
     checks the classification and the trace file.
2. **`--trace-out` adds to a trace already at its path.** A traced build
   interprets only what its trace missed, so a second cover capture of a
   rebuilt game used to overwrite `profile/cover.trace` with a few dozen
   addresses and the next rebuild lost everything the first run found.
3. **Pacing threw away a late field's debt while the sound queue was
   starved.** The storybook draws at 30 Hz: one field runs 26-30 ms and the
   next 0.4-0.6 ms. With the queue below half its target the frontend set
   the schedule to *now* after the slow field, so the free field after it
   slept a whole field - 71 fields a second of machine showed as 54-58, the
   queue hovered at the threshold (mean 36 ms against 80) and never refilled.
   Starved now means no sleep and no credit for being ahead, but the debt
   is kept (and still dropped after 250 ms behind). Windowed, the storybook
   title went from 54-58 to 60.0-60.2 paced on every 300-field window, for
   the untraced build too (uf: 377 underruns against 597).
4. **A crash the windowed runs found, in the helper pool.** Three windowed
   runs of three builds (one with main's pool and pacing, differing only in
   the coverage change, which a windowed run does not use) died with an
   access violation after 58-81 s; the fourth had a vectored handler that
   named `draw_chunks` on a helper reading a job whose machine pointer was
   garbage - a job that had already returned. `gc_workers_run` closed a job
   with a plain store and then read `inside`; a helper increments `inside`
   (locked) and then reads `closed`. A plain store can still be in the
   processor's store buffer when the caller reads `inside`, so both sides
   can miss each other and the helper runs a job from a dead stack frame.
   Closing is now `InterlockedExchange` (`__sync_lock_test_and_set`), which
   commits before the next instruction. Four windowed 90 s runs and every
   headless run since: no crash. It is timing-dependent, so four runs are
   evidence, not proof.

**Gates**, on uf/tf against main: frames at 600, 1,200, 2,000, 2,500, 3,000
and 3,700 byte-identical to main's (main's u and tf); state at 1,800 fields
through the frontend with the same card, main's u and tf both
14,594,680,000 instructions, pc `8005b59c`, hash `ab1c097185141808`;
`--differential --auto-input` 1,200 fields on tf clean;
`run_recompiled_tests.py --slow` all 11 agree; GC and hub CTest 16 of 16.

**Hub policy: unchanged.** The cover run's 3,000 fields do not reach the
title, but they already contain the entry of the function whose loops the
title interprets (`800b68d0`) and the rest of its blocks follow statically:
a 6,000-field idle run of tf is 99.991 % native (4.4 million of 48.6 billion
interpreted, almost all the low-memory vectors the OS writes at run time).
A longer cover run would cost minutes of untraced interpretation for
nothing measurable; tlf, with a 6,000-field auto-input trace, is no faster
than tf in any window and builds 55,640 blocks against 34,200 (48 s against
25-29 s). Hub traced builds take 25-29 s, untraced 20-24 s.

**What remains below full speed**: only the loading screen, fields
2,400-2,750, at 28-29 fields a second in every build (the game's own
floating-point code; a separate branch is working on it). In the window
every other 300-field window of tf runs at 59.9-60.2, and the 296 underruns
are that screen's.

### The loading screen, taken apart (2026-09-13, round five)

Round four left Shrek 2's loading screen (fields 2,400-2,700, no triangles)
at about 46 ms a field against the 16.7 ms a field the console allows, and
put that down to the game's own floating-point code plus a tenth of slow bus.
Before changing anything this round measured it three ways on a generated
build of main (`4fa86e3`), headless, SDL dummy drivers, unpaced, the same
memory card copied into every save directory: round four's sampler over
fields 2,350-2,750; counters (a measurement-only copy of the generated
project, never the tree) for every floating-point path, every quantised
conversion by type and every access that left the fast map, over
2,400-2,700; and a count of recompiled instructions by address over the
same window. The machine was shared with two other agents' runs throughout,
so absolute times are high and noisy; main measured 39-45 ms a field in the
runs below.

**Where the samples fell** (main, 7,162 samples):

| where | share |
|---|---|
| generated blocks, with the handlers inlined into them | 51 % |
| the slow bus: `translate` 6.4 %, `bus_read` 6.3 %, `bus_write` 5.6 %, the framebuffer-window and OpenGL hooks it calls on every access 4.1 % | 22 % |
| the `gc_bus_fast_*` loads and stores, which MSVC leaves out of line in blocks this size | 9.5 % |
| the dispatcher, `gc_recompiled_run` | 5.9 % |
| `gc_fp_madd_full` | 0.4 % |
| frontend, driver and the rest | 11 % |

Inside the blocks the lines that stood out were the fast path's narrowing
to single (`gc_fp_fast_finish`, 4.4 %), reading MXCSR for the host's
rounding mode (2.6 %, twice an instruction for a paired one), the
dequantise and quantise conversions (6 %), and the single-precision FPRF,
which narrows the result a second time to classify it.

**By guest block**: `802570b0`, the decoder's 8x8 inverse transform (52
instructions a pass: 8 `psq_l`, 24 `ps_add`/`ps_sub`, 5 multiplies, 6
quantised stores), 19.4 %; `80257c70`, colour conversion (`psq_lu`,
`ps_madd`, merges, `psq_stu`), 15.3 %; then `80259ec4` 4.5 %, `80256fe0`
4.5 %, `80256ed8` 3.2 % and `80259e48` 2.9 %. The integer blocks of the
bitstream reader run 1.1 M instructions a field for about 7 % of the time;
the two floating-point blocks run 1.3 M for a third of it.

**By handler and path** (counts a field):

| what | a field | path |
|---|---|---|
| recompiled instructions | 6.9 M | `addi` 15 %, `lwz` 11 %, `bc` 9 %, `stw` 5 %, `b` 5 %; paired and quantised about 22 % |
| single add / subtract / multiply lanes | 445 k / 386 k / 99 k | all but 3 in 300 fields on the fast path |
| single multiply-add lanes | 465 k | 98.8 % fast; 5,400 the full way |
| full-path arithmetic | 126 | every one in non-IEEE mode, which is on throughout |
| divide, double precision, `frsp`, `fctiw`, `fres`, compares | about 200 together | negligible here |
| dequantise lanes | float 249 k, u8 293 k, s16 51 k | |
| quantise lanes | float 220 k, u8 513 k | |
| `ps_round` (moves and merges) | 180 k | |
| loads that left the fast map | 1.11 M | all at `0x811xxxxx`-`0x817xxxxx`, all through DBAT2 |
| stores that left the fast map | 0.76 M | the same |
| page-table translations | 0 | |

So the "virtual memory pages" were not pages at all. Shrek 2 maps 8 MB at
`0x81000000` with DBAT2, and the fast map only took a BAT that covered a
whole 16 MB piece, so every load and store in the decoder's buffers - 1.9
million a field - walked four BATs, the locked cache, the register block and
the framebuffer window. And the floating-point work that is left is all on
the fast path already: what costs is the path itself, not the flags.

**Where it got to.** Headless, dummy drivers, unpaced, the same card, an
instrumented copy of each build (the same instrumentation in all), three
rounds interleaved on a quiet machine; wall milliseconds a field of run and
present, and the main thread's cycles a field over the loading screen:

| build | loading, 2,400-2,700 | main-thread cycles | intro 0-1,700 | storybook 3,100-3,700 |
|---|---|---|---|---|
| main `4fa86e3` | 34.6 ms (34.6-34.9) | 130.0 M | 3.72 ms | 14.5 ms |
| the fast map takes a BAT's prefix | 23.8 ms (23.8-23.9) | 89.3 M | 3.18 | 13.8 |
| paired and quantised bookkeeping | 18.9 ms (18.9-19.0) | 70.6 M | 3.20 | 13.9 |
| quantised lanes as one vector | **18.4 ms** (18.3-18.5) | 68.8 M | 3.21 | 13.9 |

Main measured 34.6 ms rather than round four's 46 because that figure was
taken on a busy machine. The loading screen is 1.88 times faster and still
1.7 ms a field short of the console's 16.7. Every change below kept state
at 1,800 and 3,700 fields (`e428f9744ecfaa22` and `8d865e879b790e6d` with
this round's card, the same instruction counts and program counters as
main), frames 600, 1,200, 2,000, 2,500, 3,000 and 3,700 byte-identical to
main, GC and hub CTest (hwtests at their baseline), every recompiled test
program, and a 2,800-field `--differential --auto-input`. The generated
build stays at 21-26 s (22 s for the last); the executable grows from
30.3 MB to 31.5 MB.

- **The fast map takes the start of a piece** (`gc_bus.c`). A BAT matching
  a piece's start maps its first 2^k bytes by the same arithmetic, k the
  ones at the bottom of its mask; the map now takes that prefix, as long as
  no earlier BAT matches anything in it. `tests/gc/gc_bus_fast_test.c`
  (CTest `gc_bus_fast`) holds the map to the bus's own walk over 20,000
  random BAT layouts - blocks smaller than a piece, not starting one,
  overlapping, masks with holes, invalid pairs - and checks Shrek 2's
  layout lands in the map; main fails it, and so do mutations that drop
  the earlier-BAT check, widen the prefix a step, ignore the supervisor
  valid bit or drop the main-memory cap.
- **Paired arithmetic as two pure lanes** (`gc_fpu_impl.h`). The fast
  path's kernels compute a lane's result and inexactness and touch nothing,
  so a paired add, subtract, multiply or multiply-add tries both lanes, asks
  MXCSR once, and commits FPSCR in one pass: the two lanes' flags, VX and
  FEX (now without branches - each exception bit sits 22 above its enable),
  and FPRF from the class a fast result must have. Otherwise the whole
  instruction runs lane by lane as before, in one shared copy a file.
- **FPSCR a note behind.** In the steady state a running game sits in - FX
  and XX set, FR clear, no exception enabled in FPSCR or MSR, no Rc - such
  an instruction changes only FI (lane 1's inexactness), FPRF (ps0's class)
  and the derived VX and FEX. So it leaves lane 1's operands and ps0 in the
  CPU and skips TwoSum's error term and FPRF; the next instruction in the
  same state overwrites the note, and anything that reads or changes FPSCR
  beyond RN and NI settles it first (`gc_fp_settle`: the FPSCR helpers,
  compares, `mffs`, `mcrfs`, the conversions, Rc, `gc_round_enter`, and
  outside the handlers `gc_cpu_fpscr` for the differential comparison and
  the state dump). Settling recomputes the lane with the same kernel in
  round to nearest and never runs between an instruction's arithmetic and
  its reading of the host's flags. On the decoder's own blocks, run alone
  on state dumped from the game, this took the inverse transform from 23.8
  to 15.4 TSC ticks an instruction.
- **Quantised loads and stores.** When the fast map covers both lanes'
  bytes, `psq_l`/`psq_st` read and write them directly with the GQR type
  decided once; the integer types drop a round trip through a single that
  cannot change their value (at most 16 bits times 2^-31..2^32); and on
  x86-64 both lanes of an integer type go through one SSE2 vector (NaN mask
  to +0, max then min against the bounds, `cvttpd`).
- **Tests.** `tests/gc/gc_fpu_fast_test.c` keeps the replaced code as its
  reference and adds: runs of one to three paired instructions in and out
  of the steady state followed by an FPSCR reader (`mffs`, `fcmpu`,
  `fadds`, an Rc form, `mtfsb0` of FI, `mcrfs`, `fctiw`, `ps_div`) in every
  rounding mode; FPSCR summaries and single classification against the old
  branches and conversion; the quantised conversions against the old ones;
  and `psq_l`/`psq_st` through the fast map against the bus over every GQR,
  with spans running off memory. 34 of 39 deliberate mutations fail it and
  one more (a store that forgets the reservation) fails `gc_bus_fast`; the
  four that survive drop a settle another settle on the same path already
  made, or the steady state's enable checks, which change nothing today
  because no floating-point program exception is modelled.

**hwtests** (`run_gc_tests.py --slow`): every `cputest` at its baseline -
`fprf`, `frsp`, `fctiwz`, `nan`, `ni`, `cr`, `load`, `mtspr`, `rlw` and
`srawix` pass, `reciprocal` 1 of 17,411 and `pairedmove` 388 of 8,800 as
before. `cputest_fctiw` gives no verdict in the runner's 600 interpreted
fields (it sweeps 771 million cases), so it was recompiled and run whole:
**all 771,883,008 subtests pass**, in every rounding mode, in 376 s - the
first complete run of it on this machine.

**Tried and dropped:**

- *`gc_bus_fast_*` forced inline* everywhere: 84.8 to 86.1 M cycles and
  5 MB of executable.
- *Kernels without branches* (compute everything, one test at the end):
  the inverse transform 25.4 to 28.4 ticks an instruction. The branches
  were predicted; the work they skipped was not free.
- *FPRF split out of FPSCR with a conditional store*, and the inexact test
  on bit patterns instead of a floating compare: no change. What cost was
  the work, not a dependency chain through FPSCR or mispredicted branches.
- *Extending the flag fast path* to double precision, divide, compares and
  the conversions: about 200 of those a field here against 1.5 million
  paired and quantised instructions.
- *A translation cache for page-table addresses*: nothing in the window
  used the page table.
- Measured, not changed, because they are the emitter's or the
  dispatcher's: parts of 16 or 64 instructions instead of 4 (85.0 to 81.8
  or 80.3 M cycles, build 39 or 76 s); per-instruction glue without the
  exception and branch checks for handlers that cannot take them (-2.7 %);
  not reading MXCSR at all (-1 %, and not exact on its own).

**What is left** (profile of the bookkeeping build): the generated
per-instruction glue 22 %, `gc_bus_fast_read32`/`write32` out of line 13 %
(the bitstream reader's `lwz`/`stw` and the `stmw`/`lmw` of its calls), the
dispatcher 7.6 %, the fast kernels about 12 % and the quantised paths 7 %.
The multiply-adds whose frA has more than 27 significant bits (1.2 % of
lanes) still take the full path. The next levers are the emitter's: the
host rounding mode known per dispatch rather than read per instruction,
longer parts for floating-point blocks, and lighter glue for handlers that
cannot branch or fault; and `stmw`/`lmw` through one fast-map check.

### Cutscenes and gameplay (2026-09-14)

The user played the hub build of main (`f1e8a7b`) and found everything up
to the storybook at full speed and the game slow once it reached its
in-engine cutscenes and play. Nobody had measured that part: every earlier
round stopped at the title.

**Getting there.** `--auto-input` presses through the menus and is playing
in the swamp by field 1,650, skipping the cutscene. A script that presses
START/A at the auto-input's times up to field 1,300 and then lets go, with
the auto-input's play cycle from 3,600, gives, headless and without a
memory card: the "New Game" book to 1,300, the storybook narration to
2,350, the in-engine cutscene (Shrek and Fiona at the swamp, subtitles) to
3,550, then play. The frontend follows the same timeline field for field
when the instrumented copy is given the script and no card (a card changes
the menus). All measurement below used a copy of the generated project
with a per-field log and the round-four sampler, never the tree; builds
are hub-like, traced with the 3,000-field no-input cover run.

**Where the lag was** (main, headless runner, unpaced, quiet machine, fields
a second of run and present):

| fields | scene | main | ms a field | of which gx stream | raster | transform | processor |
|---|---|---|---|---|---|---|---|
| 0-2,100 | intro, menus, narration | 84-338 | 3-12 | | | | |
| 600-900 | loading (scripted run) | 43.8 | 22.8 | | | | 22.3 |
| 2,100-2,400 | cutscene starts | 31.1 | 32.2 | 24.9 | 21.1 | 1.6 | 7.2 |
| 2,400-3,000 | cutscene | 16.3-17.3 | 58-61 | 49-51 | 42.5-44.1 | 3.0-3.3 | 8.8-9.8 |
| 3,000-3,600 | cutscene ends, play starts | 12.6-14.5 | 69-79 | 54-66 | 46-56 | 3.5-4.4 | 13.6-15.2 |
| 3,600-6,000 | swamp gameplay | 11.8-13.4 | 75-85 | 56-62 | 47-52 | 3.7-4.1 | 19.2-22.9 |

The windowed run (real window, WASAPI, `--mute`, paced) agreed: 17.5-27.7
fields a second in the cutscene, 13.2-15.4 in play.

What a gameplay field is: 24,000-26,500 triangles (triangle lists, three
vertices each, all from display lists, about 330 draws), of which 16,000
reach the rasteriser and 4,000 were big enough to split; 2.75 M pixels
visited, 1.65 M covered, 1.40 M combined, 450 k written. The cutscene is
15,500-17,800 triangles, 2.4 M covered, 620 k written. AX mixing costs 0.05
ms a field and EFB copies 0.17 ms: neither matters.

The sampler with the helpers turned off (`GC_RASTER_THREADS=0`), gameplay:
texture sampling 30 % (the compressed-texture footprint 10 %, the bilinear
blend 8-10 %, the level of detail 7 %), `draw_rows` 17 %, the combiner 14 %,
`finish_point` 7-10 % (most of it for the neighbour points a gradient
needs), fog's `pow` 5 % (shown as `finite`, the export before it in the C
library), transform 1.3 %, recompiled game code 5 %. With helpers on, the
main thread spent most of the raster time waking and joining helpers for
triangles of a few dozen pixels. `--coverage` on the scripted run: 98.57 %
native, 696 M instructions interpreted, nearly all in play - the cover
trace never gets there.

**Kept** (headless, fields a second; the cutscene is fields 2,400-2,700 and
play 3,900-4,200; each step is on top of the one before):

| change | cutscene | play |
|---|---|---|
| main `f1e8a7b` | 17.3 | 13.2 |
| triangles held and drawn together in bands; hidden pixels not combined; fog after the alpha test | 41.8 | 26.3 |
| held as vertices, transformed and clipped on the helpers | 45.1 | 28.0 |
| the plan worked out once while nothing moves; decoder bytes without a call | 46.0 | 32.2 * |
| setup taken eight triangles at a time; the vertex format once a draw | 46.9 | 32.9 * |
| held as the draw's bytes, decoded on the helpers; one-block compressed footprints | 51.2 | 36.2 * |
| no gradient for a texture with one level | 56.1 | 39.0 * |

\* built with the cover trace plus a 3,000-field `--auto-input` trace (see
below), which by itself took play from 28.0 to 31.3 and left the cutscene
where it was.

- **Held triangles, drawn in bands** (`gc_gx_raster.c`). While the command
  stream runs (`gc_gx_raster_begin`/`end` around `run_stream`), a draw whose
  combiner plan is independent with no indirect stage is copied as the
  bytes of its vertices. Every CP, XF and BP write and the end of the stream
  draw what is held: every triangle decoded, transformed, clipped and set up
  on the helpers (a triangle's three vertices found from its place in the
  draw, as primitive assembly picks them), then the framebuffer cut into
  bands, each band drawn by one thread through every held triangle in
  arrival order. It is exact because the only state pixels share is the
  colour and depth at their own position and a position belongs to one band;
  nothing a held triangle reads can change while it is held (registers only
  by a stream write, which flushes first - an EFB copy and a palette load
  are BP writes; main memory only when the processor runs, and the stream
  flushes before returning); every held triangle therefore has the same
  plan, whose registers a pixel either writes before reading or no stage
  writes at all; and the machine is left with the registers of the last
  triangle to run the combiner, in its highest band that did. Lines, points,
  a plan that does not split, the OpenGL path, the `on_primitive` and
  `on_triangle` hooks and draws over 2,048 triangles are drawn when they
  arrive, after what is held. About 330 flushes a field in play, 74
  triangles each.
- **Hidden pixels.** With late depth testing and an independent plan, a
  pixel the depth test will throw away is not combined: nothing the
  combiner does can change the depth it is compared against. What it would
  have left in the registers matters only if it was the last pixel of its
  rows to run the combiner, so `draw_rows` remembers the last one skipped
  and runs its combiner at the end, on points computed the same way. About
  380 k of 1.65 M covered pixels a field in play, 0.8-1.2 M in the cutscene.
- **Fog after the alpha test.** Fog never touches alpha and neither test
  reads colour, so it is only computed for a pixel that will be written.
- **One texture level, no gradient.** `level_of_detail` clamps to `max_lod`
  and then `min_lod`, so a texture with `max_lod <= min_lod` reads `min_lod`
  whatever the gradient. The plan records which coordinate sets have a
  texture with a real range (`gradients_used`); for the others the
  neighbour points are not computed and the gradient goes in as zero.
- **Smaller ones.** The decoder's byte reads test the address themselves
  instead of calling `gc_machine_ram`; the vertex format is read once a
  draw (`GcGxLayout`); a bilinear footprint inside one compressed block
  (both columns in one block of four, both rows too) is one memo lookup
  with no copy.

**Gates** on the final code (both commits), against main: frames at 600,
1,200, 2,000, 2,500, 3,000 and 3,700 byte-identical; the scripted run's
frame every 300 fields to 6,000 (20 frames, through the cutscene and play)
byte-identical; the runner's state at 6,000 scripted fields
48,648,600,001 instructions, pc `802474c0`, hash `e67a41b1b1fad209`, as
main; the frontend at 6,000 scripted fields with a copy of the same card
48,648,700,000, pc `802183d8`, `98372fad7271f183`, and without a card
(through play) pc `800a2ec4`, `c33412a5280bb380`, both as main;
`--differential --auto-input` clean for 3,200 fields on the first commit
and 3,000 on the second (play from field 1,650);
GC and hub CTest 17 of 17; `run_recompiled_tests.py --slow` all 11 agree.
A generated Shrek 2 builds in 28-34 s with the cover trace (50 s with the
auto-input trace added).

**The trace.** The hub's cover trace is 3,000 fields with no input and
never reaches play, so 1.4 % of play's instructions - 4 ms a field -
were interpreted. A 3,000-field `--auto-input` trace recorded from a
traced build and passed to `gcrecomp` with the cover trace makes the same
scripted run 99.990 % native (4.7 M interpreted of 48.6 G), with a
different input script than the one that recorded it: play's processor
time went from 18.6 to 14.1 ms a field. It costs 21,000 more blocks
(55,059 against 34,200) and a 50-second build against 29. **Proposal:**
the hub's cover capture adds a second run, 3,000 fields of
`--auto-input` with `--trace-out` to the same `profile/cover.trace`
(which appends), before the rebuild. Not done here: it is the hub's
policy, and it doubles a traced build's compile time.

**Final, per section** (headless unpaced; windowed paced, real window,
WASAPI, `--mute`, no card, the same script; fields a second):

| fields | scene | main | final, cover trace | final, cover + auto trace | main, window | final, window |
|---|---|---|---|---|---|---|
| 0-2,100 | intro to narration | 84-338 | 120-428 | 117-417 | 59.8-62.4 | 59.9-62.4 |
| 600-900 | loading | 43.8 | 52.7 | 53.0 | 52.2 | 52.5 |
| 2,100-2,400 | cutscene starts | 31.1 | 73.5 | 76.9 | 27.7 | 49.9 |
| 2,400-3,000 | cutscene | 16.3-17.3 | 51.0-54.2 | 52.8-56.1 | 17.5-18.9 | 51.9-54.0 |
| 3,000-3,600 | cutscene ends | 12.6-14.5 | 37.6-37.9 | 39.8-42.2 | 13.2-15.4 | 39.2-42.1 |
| 3,600-6,000 | play | 11.8-13.4 | 29.1-33.9 | 34.5-40.1 | 13.3 (to 3,900) | 36.0 (to 3,900) |

The final cover-trace column ran with 12-14 % load from another session's
tests; the others at 2-8 %.

**Tried and dropped:**

- *19 helpers instead of 15* (the cap raised to 31): play 34-36 against
  33-36 - nothing. Seven helpers take the band phase from 10 to 19 ms, so
  the helpers are doing real work, but more cores are E-cores.
- *Bands cut to equal bounding-box area* (up to 128) instead of equal rows,
  to shorten the wait for the last band: 1 % in an interleaved A/B, not
  worth the code.
- *Holding transformed triangles* (the first version) left transform,
  clipping, setup and decoding on the main thread: 26.3 against 39.0 now.
- **A measurement trap again**: the first "quiet" baseline, from a counting
  build that bumped shared per-pixel counters on every thread, read 4.7
  fields a second in the cutscene against 17.3 for the plain build. Counting
  builds now run with the helpers off and are never timed. And other
  sessions ran seven emulators at once for an hour of this round: timings
  from then were thrown away.

**Where the time goes now** (final with both traces, play, 3,900-4,500,
milliseconds a field on the main thread, 25-26 in all): the processor 12.8-13.1
(recompiled blocks - floating-point matrix code at `8020dd58`-`8020df90`
leads with 4 % - plus the fast bus and the dispatcher), the band phase
9.8-10.1 (its own bands, then about 2.5 ms waiting for the last helpers),
setup on the helpers 1.1-1.3, the stream and everything else about 1. The
cutscene: processor 6.6, bands 9.6, setup 0.7. The band phase is compute:
per thread the texture sampler, `draw_rows` and the combiner, in that
order. To reach 59.94 in play the processor needs the emitter levers named
under the loading screen above (its code is the same kind: `lfs`, `stfs`,
`psq_l`, `psq_st` a call and the per-instruction glue each), and the pixels
need about half their per-pixel cost taken out - the sampler's
compressed footprint and blend, and the combiner's per-stage selection
into a plan-time program, are where to look first.

### Registers in locals: the block emitter (2026-09-15)

Branch `feature/gc-recompiler-locals`. The goal: guest code 2-3 times
faster, and smaller, faster-to-build generated code. Nothing may change
that the guest can see, down to the instruction count and the state hash.

#### Profile, before

Measured on main `4ff8bd1` with a sampling build (a pc-indexed histogram
of blocks and out-of-line helpers) over the menu load (fields 600-900), the
cutscene (2,400-3,000) and the snail walk (3,600-4,800):

- The FP and paired handlers were about half of block time. Each was a call
  through `gc_op_*` that settled the FPSCR on every instruction.
- The per-instruction glue (`GC_INSN_PLAIN`/`GC_INSN_PURE`, `c->pc`, cycles,
  and a pending check after each one) was about a third.
- The out-of-line fast bus path was about 8%, and the dispatcher 4-9%.
- The menu load runs 7.16 M instructions and 1.55 M block entries a field.
  Blocks are short, so what is paid at each entry and exit matters as much as
  what is paid in the body.

#### Design

`recompilers/recompiler-gc/src/block.cpp` writes a whole block as one C function over
locals. The runtime side is `runtimes/runtime-gc/include/gc_recompiled_rt.h` and
`runtimes/runtime-gc/src/gc_recompiled_rt.c`.

- **Registers in locals.** A block loads the GPRs, CR, XER, LR, CTR and FPRs
  it reads before writing, and writes back the ones it wrote. The FPRs are
  `__m128d` lanes. Each exit jumps into a chain of labels ordered by first
  write, so an early exit writes back only what was written by then. CR and
  XER stay whole words in locals, so compare and carry cost one shift each.
- **Exact events, checked per block.** A block entry tests
  `cycles + N > next_event` once. If an event would land inside the block,
  `gc_rt_enter` does the between-block work: the field sync, and stopping at
  the until-field or on a stop. It then runs the due events and either runs
  the block or steps it with the interpreter (`step_block`). After any
  out-of-line call that can move `next_event` (a slow bus access, a
  handler), `gc_rt_resync` writes the registers back and, if needed, has
  `gc_rt_exit` step the rest of the block. Instruction counts are therefore
  exactly the interpreter's.
- **Linking.** A block returns the next block to run, and the dispatcher loop
  is `while (next) next = next(m)`. Static targets are direct references.
  `blr` checks a small return stack that calls push. The address is compared,
  never assumed. A block that branches to its own start loops with
  `goto top` while `cycles + N <= next_event`. A block reached only by
  falling through a conditional branch is written into its predecessor's
  function (64 instructions at most), with side exits.
- **Memory.** Loads and stores go through the fast map inline
  (`GC_RT_LOAD_MAP`/`GC_RT_STORE_MAP`). The slow path is one call that packs
  where it came from.
- **FP.** The FPSCR sticky and FPRF updates are kept as a lazy note in locals,
  and flushed before any handler and at exits. `fmr`, `fneg`, `fabs` and
  `fnabs`, the paired moves and merges, and float `psq_l` are written out on
  the locals. `ps_add`/`sub`/`mul`/`madd` and their variants call one shared
  exact SSE2 kernel each, `__vectorcall` with the lanes in XMM registers. A
  kernel answers all-ones lanes when it declines: a non-steady rounding mode,
  an exception enable, or an input or result where the host would differ
  from Gekko. The block then calls the handler.
- **Drift defence.** CTest `gc_recompiled_fuzz`: a generator writes random
  blocks through `emit_block`, and the driver runs recompiled code and the
  interpreter from random states. It compares the CPU (the FPSCR settled, the
  pending note exactly) and a data window. 2,000 cases × 24 rounds take
  about 5 s. Twelve deliberate emitter mutations were each caught.

#### Measurements

Back to back, main `4c894a1` (graphics thread merged) against this branch
merged with it. Shrek 2 was built with its pointer roots. The figure is
machine-thread CPU ms a field, which held steady under the 10-99% load
other processes put on the machine during these runs:

| scene | mode | main | branch | speed-up |
|---|---|---|---|---|
| menu 600-900 | software | 17.1-17.3 | 9.4 | 1.83x |
| menu 600-900 | `--gl` | 16.7-18.1 | 9.2-9.4 | 1.85x |
| cutscene 2,400-3,000 | software | 6.8-7.1 | 3.7-3.9 | 1.8x |
| cutscene 2,400-3,000 | `--gl` | 6.3-6.4 | 3.5-3.6 | 1.8x |
| snail 3,600-4,800 | software | 13.2-13.6 | 6.4-6.6 | 2.05x |
| snail 3,600-4,800 | `--gl` | 11.9-12.0 | 6.2-6.4 | 1.9x |

Wall-clock fields a second on the menu load, the one scene that is all
game code: 56-57 against 103-105. On the snail walk and the cutscene, wall
speed is now set by the graphics thread (software `gx_ms` 33-75 a field
under load). The processor alone would allow about 155 fields a second on
the snail walk and 106 on the menu load.

Generated code: the executable went from 138.5 MB to 81.6 MB. The build
went from about 101 s to 76 s (85 s inside `run_checks` under load).
Interpreted instructions a field rose from about 500 to 800-1,600: these
are the blocks an event lands inside, stepped exactly.

Where the menu load goes now: blocks 48%, `gc_rt_ps_binary_call` 16%, the
madd kernel 15%, `gc_rt_psq_store_fast` 10%.

#### What failed

- **Tail calls between blocks.** MSVC does not reliably emit them: escaped
  locals and callee-saved registers block it, and the Shrek build overflowed
  the stack. Blocks return the next block instead.
- **Inlining the paired kernels into every block.** 104 s builds for a few
  percent. One shared `__vectorcall` copy costs 83 s and is nearly as fast.
- **Rewriting the kernels' tests on 64-bit lanes** (fewer shuffles). It was
  slower: MSVC ignored `forceinline` on the helpers and saved more
  registers. Reverted.
- **`psq_l`/`psq_st` and the lazy FPSCR fully inline.** Little gain for much
  compile time. They stay off by default (`--psq-inline`, `--lazy-inline`).
- **Trace merging and the return stack** measured no gain on the menu load
  (its blocks are called from everywhere). They are kept because they cost
  nothing, and are exact.

#### Gates

On `ec75123` merged with main `4c894a1`:

- **Scripted run.** Shrek through `play.txt` to field 6,000, frames every
  300 fields: 20/20 frames identical, and the state hash and 48,648,600,001
  instructions identical to main, in software and `--gl`.
- **`run_recompiled_tests --slow`:** all 11 agree (113 s).
- **`run_checks --quick`:** build, recompiled tests, game build, frames at
  600-3,700 and audio all passed. `gc_split_wait_loop` failed because it
  looked for the old `void gc_block_` signature, so its "absent block"
  checks could never fail. With the test fixed, the GC CTests pass 20/20,
  including `gc_hwtests_baseline` (110 s).
- **`--differential` from saved states:** clean over 600-1,200,
  2,400-3,000, 3,000-3,600, 3,600-4,200 and 4,200-4,800. That is 3,000
  fields in 137-251 s a chunk.

#### Left open

- The paired kernels are still a call each. A block-level paired sequence
  (kernel inputs kept as lanes across instructions) would remove the
  per-call check.
- `psq_st` out of line is 10% of the menu load.
- Native SDK routines (`memcpy`, `OSMatrix`, `PSMTX*`), recognised
  generically by their code rather than by name, were not started.
- Events that another thread changes asynchronously must reach
  `next_event` at deterministic sync points. A block sees a changed
  `next_event` only at its entry or after an out-of-line call.

### Not done yet

REL modules, which are linked at run time and so have to be recompiled per
module and installed when the module is linked; and packing disc content into
`ID.gcdata`, which the reader can already produce but the emitter does not
yet write.

## The OpenGL path

`runtimes/runtime-gc/src/gc_gl.c` (the machine's side: batches, textures, the
register carry, copies), `gc_gl_render.c` (the render and readback
threads, every OpenGL call), `gc_gl_shader.c` (the program text),
`gc_gl_programs.c` (compiling, the compile thread, the disk cache),
`gc_gl_cover.h` (the reference's coverage, shared), `gc_memory.c` (main
memory viewed twice, with page holds) and `gc_gl_internal.h`. Selected
with `gc_run --gl`, a generated game's `--gl` or `--renderer opengl`
(with `--gl-shaders` and `--gl-cache`), the frontend's Renderer setting,
or the hub's GameCube renderer setting; built only where SDL2 is present,
and needing OpenGL 4.1 core. It is a **fast path, not a second
opinion**: the reference rasteriser is what the hardware tests are held to
and therefore what "correct" means, and this is right exactly insofar as it
agrees - which, since 2026-09-14, is byte for byte on every test program,
on two hardware sweeps, on random pipelines, and on Shrek 2.

Since round two (below) the embedded framebuffer lives on the card: it goes
up once, EFB copies are made from the card and reach main memory on a
thread of their own, clears happen on the card, and it comes back only
for a peek, a write from outside, a save state or a hash. On Shrek 2 the
path is now faster than software in every segment measured - 1.2x
loading, 1.9x in the cutscene, 1.7x in gameplay, headless - and still
byte for byte the same machine.

### The OpenGL path, exact (2026-09-14)

The first version let the card rasterise and interpolate, shifted the
geometry by a twelfth of a pixel to move the card's sample point onto this
machine's, and ran the combiner as integer shader code. It agreed with the
reference only where the numbers were round: with compare_gl.py made
honest (below) it failed `gxtest_rasterization` 514 subtests of 519, and it
had no blending, logic operations, destination reads or dynamic indirect
matrices at all.

**So the card no longer rasterises the triangle.** It draws the triangle's
bounding box - the reference's own box, clamped to the scissor, as a
pixel-aligned quad, so exactly those pixels get a fragment - and the
fragment shader asks `gc_gx_raster_one`'s questions of each pixel:

- the vertices arrive in a buffer texture as the reference's own doubles,
  bit for bit, one 25-texel record a triangle, and the shader computes in
  **doubles** (GLSL 4.0): the same three edge functions at `px + 7/12`,
  the same inside test, the same barycentric division and perspective
  correction, the same `(int)clamp(v + 0.5)` for colour, the coordinate
  sets as `(float)` of the double sum, and the gradient from the points one
  pixel right and one pixel down, exactly as the remembered rows compute
  them;
- depth goes into a `DEPTH_COMPONENT32F` buffer as `n / 2^24`, which is
  exact for every 24-bit integer in both directions, so the card's own
  depth test compares the reference's integers;
- the combiner, the texture unit, indirect lookups, fog, the alpha test and
  `gc_gx_write_pixel` - blending, subtract, logic operations (including the
  way a negative `~s` spills into the neighbouring channel when the word is
  packed), dither, constant alpha, the two channel enables, and
  `gc_efb.c`'s rounding to the pixel format - are transcribed expression
  for expression, with every float and double `precise` so the compiler
  may neither fuse a multiply-add nor reorder a sum (MSVC is held to the
  same with `/fp:strict`);
- textures go up as `RGBA8UI` levels decoded on the CPU by the reference's
  own `gc_gx_texture_texel`, and the shader does wrap, level of detail,
  bilinear and trilinear itself with `texelFetch`;
- a pixel that needs the colour already there - subtract, a blend factor
  that names the destination, a logic operation that reads it, early depth
  with a depth write and an alpha test that can fail (the depth is written
  and the colour is not, which one fragment can only say by writing the old
  colour back), or a channel enable under a narrow pixel format - reads a
  copy of the colour buffer taken just before its batch is drawn. Two
  triangles in such a batch never overlap: an overlapping triangle ends the
  draw and the next one takes a new copy.

What the card still does itself is nothing that can differ: the depth
comparison of exact values, `glColorMask` where the rounding leaves the
masked channels as they were (the eight-bit formats), and the copy.

**Blending and logic operations are not fixed-function**, deliberately:
GL's blend computes `s*f/255 + d*g/255` in normalised floating point, and
this machine computes `(s*f + d*g) >> 8` in integers, so no factor setting
reproduces it; logic operations on the stored bits would be exact, but the
dither, the constant alpha and the rounding all happen after them, so they
go through the destination copy too.

**The dynamic indirect matrices, anisotropy and the level-of-detail bias
clamp** are done exactly as the reference does them, which for all three is
"not modelled": a dynamic S or T matrix wraps the coordinate and adds no
offset, and neither the anisotropy field nor the clamp bit is read. When the
reference learns them, the transcription follows.

**What the card cannot do** is carry the combiner's colour registers from
one pixel to the next, which the reference does. Two cases:

- a pipeline in which some stage reads a register group that a stage writes
  before this pixel has written it (`gc_gx_tev_plan_independent` says no)
  depends on the pixel before it. Those triangles go to the reference
  rasteriser: the framebuffer comes back, `gc_gx_raster_one` draws the
  triangle, the framebuffer goes up again. Slow, and exact; Shrek 2 has
  none so far, the fuzzer makes plenty.
- after an independent pipeline the registers hold what its **last**
  pixel computed, and a later pipeline may read that. `carry()` finds the
  last covered pixel on the CPU and runs the reference combiner on it. The
  one thing it cannot see is the depth buffer: with early depth testing on,
  the last covered pixel might fail the test and never run the combiner.
  That case is assumed away, counted (`carry_unchecked`), and the groups it
  wrote are marked suspect until written again with nothing assumed; a
  pipeline that reads a suspect group is counted (`suspect_reads`). The
  fuzzer's register mismatches are all in this case (247 of 6,000 runs,
  every one with early depth and the test on); **Shrek 2 reads a suspect
  group zero times** in 3,700 fields.

### Three kinds of program, and one text

The pipeline's shape - how many stages, each stage's two combiner
registers, its order and indirect command, the indirect scales and order,
the swap tables, the alpha compare, the fog function - is 80 words, read in
the shader through `CFG(i)`. The **uber** program defines that as a lookup
into a uniform block, so one compiled program draws anything; a
**specialised** program defines it as a constant array and the compiler
folds every branch away. Everything else is a uniform in both. The
arithmetic left after folding is the same arithmetic, which is what makes
the two bit-identical, and the gates check it rather than assuming it.

`GC_GL_SHADERS` (or `--gl-shaders`) picks:

| mode | a new pipeline draws with | compiled |
|---|---|---|
| `specialised` | its own program | on the drawing thread, the first time it is drawn |
| `uber` | the general program | once, at start |
| `hybrid` (default) | the general program until its own is linked | on a second thread whose context shares objects with the drawing one |

`GC_GL_CACHE` (or `--gl-cache`) keeps compiled programs with
`glGetProgramBinary`, keyed by a hash of both shaders' complete text - which
includes the pipeline, so it is also the build hash for everything that can
change a program - and of the driver's vendor, renderer, version and GLSL
version. A binary the driver refuses is compiled again and overwritten
(`disk_rejected`). Beside them `<id>.pipelines` records every shape a run
used (the generated game uses its own name); the next start compiles all of
them before the first field in `specialised` mode and queues them all for
the thread in `hybrid`. `GC_GL_CACHE=off` wins over everything, which is
what compare_gl.py sets so one mode's programs cannot stand in for
another's.

**The frontend and the hub** (round two): a generated game keeps its cache
beside itself (or in `--save-dir`) as `<id>.pipelines` and
`<id>.<16 hex>.glbin`, and the hub keeps those files through a rebuild
with the memory card and save states, so a rebuilt game hands every
pipeline it has drawn with to the compile thread at start. The hub does
not run the game ahead of time to fill the cache; with the driver
compiling in parallel (below) a cold start builds Shrek 2's 134 gameplay
programs in under 8 s while the general program draws.

### Textures

Each bound map is looked up by a 64-bit hash (xxHash64's structure) over
its address, size, format, palette binding, the highest level a pixel can
read, **exactly the bytes those levels occupy**, and for C4, C8 and C14X2
the palette entries the indices can reach. Levels above
`max(min_lod, max_lod) >> 4` plus one are never sampled and are neither
hashed nor decoded - hashing them would make every write to whatever
follows a texture in memory look like a change to it. Hits are free; a miss
decodes and uploads once; the least recently used of up to 1,024 textures
or 1 GB goes first. Because a game may rewrite texels without touching a
register, the bound maps are hashed again whenever the processor has run
since they were bound (`revalidations`).

**CMPR is not uploaded as BC1**, because it cannot be exact: the GameCube
interpolates its endpoints with weights of 3/8 and 5/8 rather than thirds,
and its three-colour mode's fourth entry is the midpoint made transparent
where BC1's is black - so no driver's BC1 decode produces the reference's
texels, and the shader's own four-texel blend would then blend the wrong
values. It is decoded on the CPU once and cached. An exact alternative, not
done: upload the raw blocks as an integer texture and decode them in the
shader with the reference's arithmetic.

Over Shrek 2's first 3,000 fields (specialised): 9,640 hits, 1,793 misses,
132 ms of hashing, 2.5 s of decoding - most misses are the storybook's
render-to-texture pages, copied back into memory each frame.

### What cost time, and would again

- **`packDouble2x32` and `unpackDouble2x32` are declared the wrong way round
  in NVIDIA's compiler (616.64).** The shader would not compile with the
  specification's spelling ("implicit narrowing of type from uvec2 to
  float64_t"). The path now compiles a two-line probe at start and uses
  whichever spelling the driver accepts.
- **The card's single-precision division is not always correctly
  rounded.** One pixel in 2,000 random pipelines came out a step darker:
  the coordinate scale `26.0 / 11.0` was one unit in the last place high.
  The scales are divided on the CPU now and handed over as float bits. The
  double-precision divisions (barycentrics, fog, the indirect offset) have
  shown no such case in 21,000 fuzzer runs, but that is evidence, not
  proof.
- **`precise` is load-bearing.** Removing it made `gc_tevtest` fail all six
  subtests.
- **A one-texel texture has a second level.** The reference's level walk
  moves on one level before it notices a level is one texel square, so a
  texture that starts one by one has a distinct level 1 in memory. OpenGL
  has no level 1 for a one-texel texture; both texels go side by side.
- **Nested sampling calls explode the compile.** The first shader inlined
  four taps into each of five level choices into each of two call sites:
  the general program took 5.7 s to compile and a specialised one 2.9 s.
  One call site per stage, and loops over taps and levels instead of
  copies, brought them to 0.47 s and 0.25-0.4 s.
- **Bind a texture after looking it up, not while.**
  `glActiveTexture(unit); glBindTexture(..., texture_for(...))` evaluates
  the upload - which switches to the scratch unit - between the two calls,
  and every map is bound to the wrong unit.
- **The framebuffer window's handlers synchronised before checking the
  address.** `gc_efb_read` and `gc_efb_write` are asked about every access
  that is not main memory, so under the OpenGL path every load and store
  brought the framebuffer back or invalidated the card's copy, and
  `gc_bus.c` turned the fast memory map off because of it: 400 fields of
  Shrek 2 made 1.1 billion synchronise calls and took 77.8 s. They check
  the address first now, the map stays on, and the same run takes 2.1 s.
- **Do not flip y** (still true): the framebuffer copied up from memory
  and the pixels drawn over it must agree about which row is row zero.
- **A gate that cannot fail.** `compare_gl.py` ran `gc_run` without
  `SDL2.dll` beside it; both runs died before `main`, printed nothing,
  and "agreed". It now fails a run that crashes or exits differently,
  compares frames byte for byte instead of within a tolerance, and
  `gc_run` gets the DLL copied next to it by CMake.

### The gates, and what they gave

All on the RTX 5070 Ti, driver 616.64, with another agent building Shrek 2
on the same machine; times are wall clock.

| gate | result | time |
|---|---|---|
| `compare_gl.py`, 16 programs (gc-programs and gxtest), frame **and** verdicts, byte for byte | 16 of 16 agree in each of `specialised`, `uber`, `hybrid` (the old path: 17 of 18, `gxtest_rasterization` failing 514 of 519; and before the script was fixed, every program "agreed" vacuously) | 174 s, 12 jobs |
| `gxtest_tev` to completion (7,352 fields), uber | stdout byte-identical to the reference: 63,489 + 65 subtests, the reference's own 25 failures | GL 24.7 min, reference 30.6 min |
| `gxtest_lighting` to completion (3,793 fields), hybrid | identical, 65,536 pass | GL 11.7 min, reference 15.6 min |
| `gc_gl_fuzz`, random pipelines from the same framebuffer, every colour and depth pixel | after the two fixes below, **0 differences** in 2,000 uber (seed 7), 6,000 uber (12345), 5,000 uber twice (99, 2024), 3,000 hybrid (31) and 300 specialised (32); before them the three modes produced the *same* single-pixel difference at seed 7, which is the modes agreeing with each other; register mismatches only in the flagged early-depth case | 26-75 s uber per 5,000; specialised 57 s per 300 (compiling) |
| CTest `^gc_` | 17 of 17 | 114 s |
| `run_recompiled_tests.py` | 10 of 10 agree with the interpreter | 130 s |
| Shrek 2, generated game, **software** path, fields 600, 1,200, 2,000, 2,500, 3,000, 3,700 | byte-identical to the reference frames | 76 s, 6 at once |
| Shrek 2 through **the OpenGL path**, same fields | **byte-identical in all three modes** | uber 60 s, hybrid 53 s, specialised 45 s (6 at once) |

The fuzzer found two real disagreements before it found none - the
division and the one-texel second level, both below - and the blending,
logic, dither, format and destination-copy code agreed from its first run.

### Measurements

**Compiling.** Cold, with a nonce in the text so the driver's own shader
cache cannot answer (`GC_GL_NONCE`): the general program 0.47-0.52 s; a
specialised program 0.25 s (gc_textest), 0.18 s on average for Shrek 2's
twelve, 0.41 s on average over 131 random fuzzer pipelines. Without the
nonce NVIDIA's own cache usually answers in a few milliseconds, which is
why a measurement has to use one.

**The hitch, Shrek 2 to field 3,000, one process at a time:**

| run | compiling on the drawing thread | worst field: compiling / all GL work | notes | wall |
|---|---|---|---|---|
| specialised, cold | 2,208 ms (12 programs, 7 fields) | **886.8 ms** / 914.3 ms, field 2,698 | | 22.0 s |
| hybrid, cold | 0 ms (the general program: 331 ms before the first field) | **0 ms** / 29.2 ms | specialised took over within 1.6 s; 1,516 of 11,762 batches drawn by the general program | 19.9 s |
| hybrid, cold, filling the disk cache | 0 ms | 0 / 25.8 ms | 13 binaries written | 20.0 s |
| hybrid, **warm** from the disk cache | 0 ms; start 8.3 ms | 0 / 25.4 ms | all 12 loaded by the thread before they were needed: 0 batches on the general program | 19.7 s |
| specialised, **warm** | 0 ms; start 22.9 ms (12 loaded before the first field) | 0 / 26.1 ms | | 19.5 s |

**Speed (first round).** Back to back, 3,000 fields: the software path 18.4 s and 17.6 s,
the OpenGL path (specialised) 20.0 s. The card was not faster then, and the
statistics say why: of its 7.9 s, 4.5 s is bringing the framebuffer back
(2,072 downloads), 3.3 s is setting batches up (2.5 s of it decoding
textures), and 62,496 of 63,668 draws needed a destination copy, which
splits nearly every triangle into its own draw. Round two removed all
three; its numbers are below.

### Hooks the GPU renderer needs from gc_gx.c, gc_gx_raster.c and gc_gx_draw.c

None of these files was edited this round (another change is holding
triangles and adding a per-draw vertex layout there). What the OpenGL path
uses today, and what it needs:

1. **`gc_gx_raster.c`: export `void gc_gx_raster_one(GcMachine *m, const
   GcGxPoint points[3])` in a header.** It is declared by hand in `gc_gl.c`
   (`cpu_triangle`, for pipelines whose pixels depend on each other) and in
   `tools/gc_gl_fuzz.c`. If it is renamed or made static the path loses its
   fallback.
2. **`gc_gx_raster.c`: keep the dispatch** `if (gc_gl_active(m)) {
   gc_gl_triangle(m, points); return; }` at the top of `gc_gx_raster_one` -
   after clipping, before set-up - or, when triangles are held, replace it
   with **`void gc_gl_triangles(GcMachine *m, const GcGxPoint *points,
   unsigned triangles)`** at the point the held triangles are released. The
   path reads `screen[0..2]` (doubles), `clip[3]`, `texcoord[k][0..1]` and
   `color[0]` of each point and nothing else; a new vertex layout must keep
   those or say where they went.
3. **`gc_gx_draw.c`: `void gc_gl_draw_begin(GcMachine *m, unsigned
   primitive, unsigned vertices)` and `void gc_gl_draw_end(GcMachine *m)`**
   around each primitive command, once its vertices are transformed and
   before triangles are assembled. With them the path can end a batch at a
   draw boundary instead of hashing about 250 register words a triangle and
   re-hashing bound textures whenever the processor has run, and can turn a
   strip, fan or quad list into one indexed record block.
4. **`gc_gx_draw.h`: the per-draw layout as a question** - which coordinate
   sets and which colour channels this draw's vertices carry - so a record
   holds only live attributes (today every triangle carries all eight
   coordinate pairs).
5. **`gc_gx.c`: `void gc_gl_bp_written(GcMachine *m, unsigned reg)`** from
   the BP register write, and the same for the XF and CP writes that reach
   the pixel pipeline. That replaces the per-triangle state hash with dirty
   bits, and a TEV colour register **load** (0xe0-0xe7, bit 23 clear) is the
   only thing that can clear a suspect carried group exactly - a reload of
   the value already there is invisible to a hash.
6. **`gc_gx_raster.c`: the pixel count** (`gx->pixels`). The path does not
   count covered pixels. Nothing reads the count except statistics today;
   if the performance counters ever return it, the path needs a hook to add
   a count (an occlusion query a batch would give it).

`gc_gx_tev.c`, `gc_gx_texture.c`, `gc_gx_blend.c` and `gc_efb.c` are used
through their existing functions (`gc_gx_tev_prepare`,
`gc_gx_tev_run_planned`, `gc_gx_tev_konstant`, `gc_gx_texture_read`,
`gc_gx_texture_level`, `gc_gx_texture_texel`, `gc_gx_texcoord_scale`,
`gc_gx_alpha_test`, `gc_gx_early_depth_test`), and `gc_efb.c` calls the path
at every read, poke and copy of the framebuffer.

### Design: keeping the framebuffer on the card

The first round's plan, kept as it was written. Round two built the copies
from the card (read back asynchronously rather than encoded on the card),
clears on the card, batches from held draws, grown triangles and the
render thread; it did not build single-pixel peeks, card-backed main
memory or copies encoded in a shader. What was built, and how it differs,
is in the next section.

**Only bring back what is read.** A peek (`gc_efb_read`) needs one pixel:
flush and read one pixel of one plane, and only fall back to the whole
framebuffer when a field has peeked many (the hardware tests peek
thousands). Track whether depth was written since the last upload and skip
the depth plane otherwise. Hooks: none in the forbidden files; `gc_efb.c`
already calls the path.

**Copies on the card.** `gc_efb_copy` is filter (three weighted row sums,
shifted, masked to nine bits, clamped), gamma, luma/chroma, then tile
encoding into main memory - all integer arithmetic that transcribes into a
fragment shader over the copy rectangle as cleanly as the pixel pipeline
did. The display copy can then be encoded and read back a line range at a
time, or presented without reading back at all. A clear after the copy
becomes a `glClear` over the rectangle with the quantised colour and the
24-bit depth as `n / 2^24`, exact, and the card's copy stays current - no
upload after every copy.

**Main memory backed by the card, read lazily.** A copy to texture memory
leaves its bytes on the card as a texture and records the address range as
card-backed. The range is brought back into memory only when the processor
reads it: `gc_bus.c` would exclude card-backed pages from the fast map and
check a page bitmap on the slow path, and a store into the range drops the
card's version. When a texture is bound at such an address, the texture
cache uses the card's copy directly (decoding the copy's tile format in the
shader, or keeping the pre-encode image beside the encoded one), so a
render-to-texture page never visits the CPU. Hooks: none in the forbidden
files; `gc_bus.c` and `gc_efb.c` would change.

**Batches built from primitives.** With hook 3, a draw's triangles go into
one record block and one instanced draw per register state. Batches that
read the destination can stay whole where the triangles of a strip do not
overlap except on shared edges - which the reference draws twice when a
sample lands exactly on one, so the split has to stay conservative there -
and a conservative triangle (the triangle grown by a pixel) instead of its
bounding box would halve the fragments of thin diagonal triangles.

**A render thread.** The context moves to its own thread; the CPU side
appends (shape, uniform block, texture keys, records) to a ring and the
thread draws. Every synchronisation point - a peek, a copy, a card-backed
read, a CPU fallback triangle, the end of a field when presenting - waits
for the ring to drain. Texture bytes are hashed at submission and decoded
on the CPU side when missing, so the thread never reads guest memory that
the processor may be writing. The combiner register carry stays on the CPU
side, where it is computed today.

### Round two: the framebuffer on the card, a render thread, the frontend and the hub (2026-09-14)

**Batches from held draws.** The rasteriser holds draws for the OpenGL
path as it does for its helpers (any pipeline whose plan is independent)
and hands over the set-up pieces in one call, `gc_gl_pieces`; records are
filled on the helpers and the combiner registers are carried once, from
the last covered piece. This did edit `gc_gx_raster.c` (`hold_draw`, the
flush) and `gc_gx_draw.h` (`GcGxRasterPiece`, `gc_gx_raster_one`), which
the hooks list above predates. The card rasterises each triangle grown by
half a pixel, computed in doubles, instead of its bounding box, and the
record reaches the fragment stage as 25 flat varyings. Specialised
programs declare conservative depth (`depth_greater`/`depth_less`,
`GL_ARB_conservative_depth`) against a per-triangle bound with a margin of
16 units, so the card can reject hidden pixels before the program runs.

**Layers instead of one draw per overlap.** A batch that reads the
destination is cut into layers in which no two triangles cover a common
pixel by the reference's own coverage (`gc_gl_cover.h`: the same edge
functions and sample point, a row's span found by a galloping search from
the row above's). Up to 384 triangles are tested in pairs by row spans;
above that a per-pixel layer map. Each layer takes one copy of the colour
buffer and one draw. In 100 gameplay fields the destination copies went
from 245,265 to 34,195.

**EFB copies from the card, without waiting for it.** A copy reads its rows
(the rectangle and one either side, for the filter) with `glReadPixels`
into a pixel buffer, sets a fence and flushes, in the drawing context. A
readback thread with its own shared context maps that buffer - the map is
where the wait for the card actually happens - and runs `gc_efb.c`'s own
copy code (filter, gamma, luma/chroma, tile encoding) on the rows, writing
main memory through a second view of it. Main memory is a Windows file
mapping viewed twice (`gc_memory.c`): the pages a pending copy will write
are **held** - made inaccessible in the machine's view - and a vectored
exception handler makes any access to them wait until the copy has
released them, so every read sees what an immediate copy would have left.
A DMA disc read settles its range first (the kernel reports a held page as
an error instead of faulting), and so do save states and hashes. The first
and last pages a copy writes are usually shared with memory the game is
still using, so when a copy's rows are independent those rows run first
and the two pages are released before the rest. Clears after a copy are a
scissored `glClear` with the quantised colour and the 24-bit depth as
`n / 2^24`, exact. Elsewhere than Windows, or with `GC_GL_SYNC_COPIES`,
copies wait for the card as before (not measured).

**A render thread.** `gc_gl.c` now runs only on the machine's thread and
touches only the machine; `gc_gl_render.c` owns the drawing context and
runs commands in order: STATE, DRAW (triangles and records inside the
command), TEXTURE (decoded texels inside), UPLOAD, DOWNLOAD, COPY, CLEAR,
SYNC. Blocks come from size-class free lists; the machine waits only for
an answer (a download, a sync) or when 512 MB of commands are queued. The
thread looks for work for 2 ms before sleeping, and the machine signals it
only when it sleeps - waking it for every command cost the machine more
than the looking costs the thread. The compile and readback contexts are
created on the machine's thread, where the windows live, and handed over.

**The shader, cheaper and still exact.** Bilinear sampling wraps its two
columns and two rows once rather than per tap, and the fog `pow2` series
stops after the i = 16 term (every later term is below half an ulp of the
sum, so adding it never changed the result). Uber 44.8 to 46.1 fields/s,
warm hybrid 49 to 52.4 (gameplay 3,600-4,200). With a trivial fragment
shader the same run reached 55.8, which bounds what shader work alone can
still buy.

**Compiling in parallel.** With `GL_KHR_parallel_shader_compile` or the ARB
extension the compile thread asks the driver for as many threads as it
gives, starts every queued program (up to 64) at once and publishes each
as its completion status turns true; the vertex shader is compiled once
per thread. Cold (a nonce in both shaders, empty cache), gameplay
3,600-4,200: all 134 programs in 7.8 s instead of about 44 s, and 46.0
fields/s instead of 35-36 while they compile; warm 52.6. Capping the
driver's threads (`GC_GL_COMPILE_THREADS`) at 10 or 4 measured within the
noise of no cap, 2 fell to 37.0. Stopping the path deletes programs still
building rather than waiting for them.

**The frontend.** Graphics > Renderer (Software or OpenGL) in the overlay
menu, kept in `gc_prefs.ini` as `display.renderer`; `--renderer
software|opengl` or `--gl` for one run; hybrid shaders unless
`--gl-shaders` says otherwise; the cache beside the game or in
`--save-dir`, or `--gl-cache DIR|off`. Switching between fields is safe
either way: the path brings the framebuffer and every pending copy back
before it stops. The picture is still presented through SDL's renderer
from memory; while a display copy is on its way the previous picture stays
up for a field rather than waiting (`gc_efb_display_ready`). For
measuring: `--unthrottled` (fast forward for the whole run) and
`--play-input SCRIPT` (port 1 plays a headless `--input` script, so a
window is timed on the same play; the run ends on the same state hash as
the headless one, in both renderers).

**The hub.** Settings > Launching > GameCube renderer: the game's own
setting (default: nothing passed), Software or OpenGL, passed as
`--renderer` for that run. `<id>.pipelines` and `<id>.<16 hex>.glbin` are
persistence files, kept through a rebuild.

**Tried, measured, dropped:**

- *EFB copies*: a staging texture read back from the second context came
  back one copy behind whatever the fences said; a flush deferred to the
  end of the command stream moved the same 1.5 ms later (and once
  deadlocked, when the main thread faulted on a held page while hashing a
  texture before handing the copy over); mapping the pixel buffer or
  `glGetBufferSubData` in the drawing context waited about 14 ms a copy for
  the driver's queue; persistent and client-storage mappings read stale
  bytes every time. On NVIDIA 616.64 `glReadPixels` into a pixel buffer
  costs about 1.5 ms headless and up to 12 ms with a visible window, and
  `glClientWaitSync` in the second context returns at once - only the map
  really waits.
- *Timer queries* for the card's time measured the idle gaps between
  commands, not the work.
- *Splitting batches at every overlapping bounding box* made 245,000
  destination copies in 100 fields; a per-pixel layer map for every batch
  was slower than the drawing it saved. Pair tests on row spans below 384
  triangles replaced both.
- *Handing the whole queue over at once* (the render thread takes the list
  and returns blocks in bulk; the machine takes free lists whole): 51.8
  fields/s against 52.4 without, over three runs each. Not kept.
- *SDL's OpenGL renderer for the window* while the path draws: 28.2 s for
  1,200 windowed gameplay fields against 23.3 s with Direct3D. Not kept.
- *Presenting from the card* was not built: `present()` is 1.9 % of the
  machine thread's samples in a windowed OpenGL run (about 0.4 ms a field),
  and the display copy has to reach main memory anyway, because the game
  can read it.

### Measurements, round two

RTX 5070 Ti, driver 616.64, 20 hardware threads; Shrek 2 traced build;
one process at a time, back to back, nothing else running (checked for
`shrek_2`, `native_console_hub` and other `G3RE52` processes before each
set). Segments start from save states made on the play script and play
it on; a 10-field run from the same state is subtracted from each
wall time, so fields/s is the fields' own. OpenGL is warm hybrid (the
cache filled by one earlier run). CPU is the process's user plus kernel
time per field.

**Headless** (`--frames`, `--input`):

| segment | software fields/s | OpenGL fields/s | OpenGL / software | CPU ms/field, software | CPU ms/field, OpenGL |
|---|---|---|---|---|---|
| boot, 0-600 | 401.0 | 433.5 | 1.08x | 39.9 | 33.6 |
| loading, 600-2,400 | 97.9 | 121.2 | **1.24x** | 106.6 | 78.0 |
| cutscene, 2,400-3,000 | 50.9 | 95.5 | **1.88x** | 297.5 | 153.3 |
| gameplay, 3,600-6,000 | 33.4 | 57.4 | **1.72x** | 384.0 | 210.4 |
| gameplay, 3,600-4,800 | 34.3 | 59.1 | 1.72x | 372.7 | 210.3 |

**Windowed** (the SDL frontend, `--play-input`, muted):

| segment | software, unpaced | OpenGL, unpaced | software, paced | OpenGL, paced |
|---|---|---|---|---|
| loading, 600-2,400 | 100.6 | 117.8 | 55.9 | 57.2 |
| cutscene, 2,400-3,000 | 51.8 | **90.8** | 51.8 | **60.2 (full speed)** |
| gameplay, 3,600-6,000 | 36.5 | 52.9 | 36.9 | 53.5 |

Paced loading comes out under 59.94 in both renderers by the same amount
(about 32 s for 30 s of fields) although both run well above it unpaced;
not investigated. The window costs the OpenGL path about 1.5 ms a field
against headless. Windowed runs with sound going to SDL's dummy device
(which prints the frontend's state hash; its pacing is not used for the
table) agree between software and OpenGL at fields 2,400
(`599ddd778fe012d6`), 3,000 (`258571d6eaf3bd15`), 4,200
(`64dd4409fae0154a`) and 6,000 (`c33412a5280bb380`), and at 4,200 and
6,000 equal the headless runs' hashes.

**Where a gameplay field goes** (OpenGL, 3,600-6,000, 17.4 ms a field):
the machine thread's OpenGL work (batching, uniform blocks, texture keys,
carry, copy submission) 1.68 ms; waiting on held pages 0.54 ms; the render
thread busy 4.1 ms of its own. Sampled on the machine thread: recompiled
guest code 52 %, bus reads and writes about 10 %, the command stream 16 %,
the rasteriser flush 19 % (of which `gc_gl_pieces` 10 % and the helpers'
transforms 7 %), page-hold waits 3 %, texture hashing 2 %. Texture decoding
is 25 ms in 2,400 fields; hashing the bound textures again after the
processor has run, 865 ms. Both renderers keep about 12 cores busy: the
helper threads spin while they wait.

**The hitch** (hybrid; worst field by the wall clock, first three fields
of a run excluded; cold = a nonce in both shaders and an empty cache):

| segment | cold: worst field, fields over 34 ms / 50 ms | warm: worst field, fields over 34 ms / 50 ms | cold compiling |
|---|---|---|---|
| loading | 88.8 ms (2,304), 2 / 1 | 38.7 ms, 1 / 0 | 48 programs, 3.3 s on the thread; longest on the general program 1.9 s |
| cutscene | 94.7 ms (2,416), 5 / 3 | 94.6 ms (2,410), 4 / 1 | 59 programs, 4.2 s; 1.3 s |
| gameplay | 120.7 ms (3,656), 9 / 3 | 101.2 ms (3,606), 3 / 3 | 212 programs, 15.2 s; 2.5 s |

No compiling happens on the drawing thread in hybrid mode; the worst
fields are the first after a state loads (textures, the first copies) and
held-page waits behind the card - the slowest copy reached memory 261 ms
after it was asked for.

**Gates:**

| gate | result | time |
|---|---|---|
| `run_checks.py --quick --build build-gpu --disc ... --frames-ref ...` (software unchanged) | everything passed: CTest 19/19, 10 recompiled programs agree with the interpreter, the game builds, six frames identical | 148 s wall |
| `--differential --auto-input` | agree at every field, 0-1,500 from boot and 1,500-2,700 from a state saved at 1,500 | 438 s + 360 s |
| `compare_gl.py`, three modes | 16/16 in each | 191 s |
| `gc_gl_fuzz` | 0 colour and 0 depth differences: 2,000 each of uber, specialised, hybrid (seed 11), 3,000 hybrid (23), 5,000 uber and 5,000 hybrid (77); register differences only in the flagged early-depth case | 12-31 s uber; 387 s specialised |
| Shrek 2 frames at the six fields, OpenGL | 6/6 identical in hybrid and in specialised | 20 s each |
| The play script from boot to field 6,000, frames every 300 fields and state hashes, software against hybrid | 20/20 frames identical; memory and full state hash identical | 480 s / 251 s, side by side |
| Gameplay 3,600-4,200, frames every 100 and hashes, after each change | identical in uber and hybrid, cold and warm | 12-27 s |
| CTest `^gc_\|hub` | 19/19 | 115 s |

### Left open

- **Gameplay is not at 59.94 yet**: 57.4 fields/s headless and 53 in a
  window. What is left is mostly not drawing - the guest's own code and the
  bus are about 11 ms of a 17.4 ms field - but the command stream, vertex
  transforms and batch building (about 6 ms together) are the next
  candidates, and held-page waits follow the card's latency (mean 20 ms
  from asking to memory).
- Both renderers keep about 12 cores busy with spinning helpers; nothing
  here reduced that. (Since reduced for OpenGL: see "Quick wins" below.)
- A peek at the framebuffer brings all of it back; card-backed main memory
  and single-pixel reads (the first round's design) are not built.
- The window is presented from memory through SDL's renderer.
- A warm start loads its binaries one at a time on the compile thread (220
  in 0.5-0.8 s), drawing with the general program meanwhile.
- Held pages and asynchronous copies are Windows-only; elsewhere copies
  wait for the card (built, not measured).
- The early-depth register carry is assumed rather than known (above).
- Pipelines whose pixels depend on each other draw on the CPU, a full
  framebuffer round trip a triangle (Shrek 2 has none).
- A cold `specialised` run still compiles on the drawing thread and hitches
  for it; hybrid, the default, does not.
- Developer switches, not settings: `GC_GL_NONCE` (a comment in both
  shaders, to defeat the driver's cache when measuring), `GC_GL_DUMP` (write
  a program that fails to compile), `GC_GL_BODY` (take the shader body from
  a file, for bisecting with `gc_gl_fuzz --only`), `GC_GL_COMPILE_THREADS`
  (cap the driver's compile threads), `GC_GL_SYNC_COPIES` (copies wait for
  the card), `GC_GL_WRITE_SOURCE` (write the general program's text).

### The snails: lag spikes in the swamp, and what an untraced build costs (2026-09-15)

The user played the hub build of `b0f2212` with the OpenGL renderer and
reported major lag spikes walking forward toward the snails in the first
swamp area. Two facts shaped everything below: the hub's Shrek 2 build had
**no trace** (its `build.cmd` passes none: 22,839 blocks, 122,333 of the
DOL's 622,776 instructions - a first build, before any cover capture), and
nobody had measured forward movement in the swamp, only the scripted play
cycle.

**Reproducing it.** `walk.txt` is the old `play.txt` to field 3,600 with the
stick held forward from there (turning at 4,250, 4,400, 4,550, 4,700, 4,850,
5,000): out of Shrek's yard, down the path past the snails and chickens at
3,900-4,100, along the fence. Runs start from save states made on that
script (3,600, 3,900, 4,100, 4,200, 4,600, 4,800, 5,100). Measurement uses a
new permanent flag, **`--field-log FILE`** in the runner and the frontend
(`gc_perf.h`): a line a field of wall time, time running the machine, the
machine thread's own processor time (`cpu_ms`, which other processes'
load inflates far less than the wall clock), instructions run and
interpreted, the command stream, the OpenGL path's work and its waits,
held-page waits and steps, textures decoded and hashed, readbacks, programs
compiled where a draw waited, disc reads and EFB copies. The game draws
every other field, so spikes are judged per two-field frame: a frame is late
past 33.4 ms.

The machine was shared with other agents' emulator runs and builds for most
of the round (often 70-100 % load, the user's own game some of the time);
every wall-clock number below was taken back to back in a window where the
process list showed nothing else running, and the rest was judged on
`cpu_ms` and instruction counts.

**Spike sources found:**

| source | how it showed | fix | before | after |
|---|---|---|---|---|
| code reached only through pointers, interpreted | 480,000 instructions a field interpreted on the walk, 1.3-2.0 million a frame by the snails; those frames 90-112 ms, 85-95 ms of it the machine thread's own processor time | code addresses in data and `lis`/`addi` pairs are roots | 486,210 interpreted a field | 562 |
| the FIFO ring's last page shares a page with a display buffer | every command written to or read from that page waited for the EFB copy to land: 48 % of the machine thread in a loaded run, whole seconds a field; on a quiet machine up to 33 ms in a frame | the ring is reached through the second view when only its page is held | 0.10-0.24 ms a field on average, up to 33 ms in one frame, quiet | 0 |
| game code reads the heap block just past the second display buffer | 32-58 bytes past its end, on the buffer's last page: each read waited for the copy (seen at 4.3 s a field when the card fell behind) | the access runs at once with its page opened, when it cannot touch a copy's bytes | ~7 waits in 900 fields | 0 (35 steps in 1,170 fields) |
| the snail area is simply heavier | 45-50 ms of machine-thread processor time a frame at 3,946-4,004 with nothing interpreted and nothing waiting | not fixed: guest code ~33 ms and the command stream ~14 ms a frame there | | |

**Windowed, paced, OpenGL** (the frontend, `--play-input walk.txt`,
`--mute`, from the 3,600 state to 4,800; "before" is `b0f2212`'s runtime
with the field log added and an untraced build, as the hub builds it;
"after" is this branch's final build; nothing else running):

| | fields/s | frames over 34 / 50 / 67 / 100 ms (of 585) | p99 frame | worst frame | fields over 20 / 34 / 50 ms (of 1,170) |
|---|---|---|---|---|---|
| before | 36.1 | 585 / 338 / 93 / 1 | 92.4 ms | 103.2 ms | 586 / 586 / 307 |
| after | 55.2 | 458 / 21 / 2 / 0 | 54.6 ms | 96.3 ms | 586 / 228 / 17 |

(The run's last frame, which includes exiting, is left out of the worst
frame. The one 96 ms frame after the change ran 50 ms on the processor and
was descheduled for the rest: `run_ms` 95.5, `cpu_ms` 50.3, no wait of any
kind recorded.) A second pair taken while the user's own game was starting
gave 34.2 against 55.0 fields a second, p99 107.4 against 56.4 ms, frames
over 67 ms 135 against 3.

**The same walk, 3,600-4,800, other ways of running** (back to back, quiet;
frames are two fields):

| | before, fields/s | after, fields/s | before: p99 / worst frame, frames over 67 ms | after: p99 / worst frame, frames over 67 ms |
|---|---|---|---|---|
| windowed, paced, software | 27.8 | 40.4 | 110.0 / 125.2 ms, 318 | 70.9 / 73.6 ms, 20 |
| headless, OpenGL | 36.1 | **60.0** | 92.5 / 141.2 ms, 96 | 53.2 / 56.8 ms, 0 |
| headless, software | 27.0 | 39.7 | 115.2 / 126.2 ms, 382 | 72.9 / 82.6 ms, 28 |

Headless OpenGL fields over 20 / 34 / 50 ms: 598 / 598 / 329 before, 598 /
228 / 12 after (the game's drawing field is always over 20). Menu load
(600-900), windowed paced OpenGL: 47.7 to 53.3 fields a second.

**Where the machine thread's time goes now** (headless OpenGL, 3,900-4,300,
sampled): recompiled blocks 45 %, waking and waiting for the helper threads
about 15 % (`SetEvent`, `Sleep`, `SwitchToThread` inside `gc_workers_run`,
from the rasteriser's `prepare_held` a flush), the command stream and its
flushes 23 % (of which `gc_gl_pieces` 10 %), the fast bus's out-of-line
loads and stores 7 %, the dispatcher 3 %.

#### The changes

- **Pointer roots** (`recompilers/recompiler-gc/src/analyzer.cpp`, `pointer_roots`).
  Every word of a data segment holding the address of an instruction and
  every `lis rD` followed within eight instructions by `addi rX, rD` or
  `ori rX, rD` building one is a root. Shrek 2: 1,522 roots, 105,777 blocks,
  568,715 instructions. A trace of the walk itself reached 713 interpreted
  instructions a field; the pointer roots alone 562. What they cost: the
  generated executable 33 MB to 132 MB (150 MB before the glue change below)
  and the generated build about 25 s to about 100 s on this machine (20
  threads, 100 files), which is the hub's first build of a game. A script
  checked beforehand what each heuristic finds against three traces: data
  pointers and register pairs together reach 3,839 of the 3,841 interpreted
  addresses of the walk and 5,375 of 5,378 of the cover trace; adding the
  address after every unconditional branch or return (a linear sweep) would
  take the rest and all 622,776 instructions, and was not done.
  `--no-pointer-roots` leaves them out.
- **The ring through the second view** (`gc_gx.c`, `gc_memory.c`). Holds
  now record the byte range each was taken for; `gc_memory_view_for` answers
  a view through which a range can be read and written without waiting -
  the second view when its pages are held but no held range covers its
  bytes. The FIFO writer and the command stream reach the ring that way,
  re-asked when `gc_memory_generation` moves (every hold and release).
- **Accesses beside a pending copy run at once** (`gc_memory.c`). On a
  fault at a held page, when the thread is the one that takes holds (the
  machine's), no helper job over main memory is running
  (`gc_memory_shared_begin/end`, `GC_WORKERS_RUN_SHARED` around every
  machine-thread `gc_workers_run` over memory - five call sites), and no held
  byte range lies within the instruction's reach of the fault, the page is
  opened, the one instruction runs with the trap flag set, and the page is
  closed again on the single-step trap if its copy has not landed. The reach
  is 8 bytes for a general-purpose integer instruction (decoded: mov,
  movzx/movsx, movbe, arithmetic, compare, test, one element of a string
  instruction) and 64 for anything else. The first version used 64 bytes for
  everything and stepped nothing, because Shrek 2's reads land 32-58 bytes
  past the buffer.
- **Lighter glue** (`recompilers/recompiler-gc/src/emitter.cpp`). Every instruction but
  the branches and `rfi` is `GC_INSN_PLAIN`: the check for what is due, the
  handler, then only whether it raised an exception or stopped the program;
  68 handlers that touch nothing but the registers they compute into are
  `GC_INSN_PURE` and ask nothing afterwards. The branch flag keeps what it
  held (only a branch sets it; nothing reads it before the next instruction
  that can branch clears it; state hashes scrub it); `npc` is still written,
  because states hash it. Menu load (600-900, headless OpenGL, quiet):
  18.8 to 16.3 ms of machine-thread processor time a field; gameplay
  4,200-4,500 about 15.8 to 14.8 ms; the executable 150 to 132 MB.

#### Gates (final build, `9f1900d` merged with main `88730ab`)

| gate | result | time |
|---|---|---|
| `run_checks.py --quick` (software frames at the six fields, CTest 21/21, 10 recompiled programs, game build, audio) | everything passed | 181 s wall (a first run failed `gc_audiotest` only because its scratch path was past MSVC's 260 characters; rerun with a short `--scratch`) |
| `run_recompiled_tests.py --slow` with the new emitter | 11/11 agree, `gc_timetest` included | 106 s |
| `compare_gl.py --modes hybrid` | 16/16 agree | 86 s |
| Shrek 2 frames at 600, 1,200, 2,000, 2,500, 3,000, 3,700, OpenGL hybrid | 6/6 identical to the reference | 116 s |
| walk.txt 3,600-5,400, a frame every 300 fields and `--state-hash`, software and OpenGL against main `88730ab` (untraced) | 6/6 frames identical; hash `f959ab957571841f`, full `7487818e25681ddb`, pc `8020da44`, 43,783,840,000 instructions in all three | 65 s / 30 s |
| the same against `b0f2212` before merging main (3,600-4,800) | 4/4 frames, hash `9c6b7e2d55e1ae28` identical | 315 / 119 / 35 s |
| `--differential` | clean: 3,600-4,100, 4,100-4,600, 4,600-5,100, 5,100-5,600 (the walk) and 600-1,000 (menu load) on the final build, 2,400 fields; 2,400-3,000 and the walk to 5,600 on the pointer-roots build | 106-292 s a chunk |
| windowed frontend at 4,800, before and after, OpenGL | hash `9c6b7e2d55e1ae28` in both | |

(Merging main changed the state from field ~4,000 on - main's own audio and
card rounds - so the frames at 4,200-4,800 differ from `b0f2212`'s and match
`88730ab`'s.)

#### Tried and dropped

- *Preparing held triangles on the calling thread only* (no helpers for
  `prepare_held`): gameplay 58.8 to 49.6 fields a second headless; a
  threshold of 64 triangles measured the same as 8, 256 worse.
- *Parts of 16 instructions* on top of the light glue: menu load 15.75 to
  15.2 ms (-3.6 %) for a build of 153 s against 97 s. Not kept.
- *The first light glue*, which kept the full glue inside an `if` for when
  something was due: the executable grew to 230 MB and the build to 3 min
  25 s for 0.5 ms a field. Folding the due check into one shape (`GC_INSN_DUE`)
  gave the same speed at 132 MB.
- *Profiling the windowed frontend with the sampler* hung twice (the sampler
  suspending the thread inside the driver); the field log was used instead.

#### After merging main `4bbf919` (helpers that sleep)

Gates again on the merged tree: `run_checks.py --quick` everything passed
(CTest 21/21, 10 recompiled programs, six frames, audio; 214 s wall);
`compare_gl.py --modes hybrid` 16/16 (85 s); Shrek 2 OpenGL frames 6/6
(18 s); walk.txt 3,600-5,400 software and OpenGL against main `4bbf919`
untraced: 6/6 frames, hash `f959ab957571841f`, full `7487818e25681ddb`
(92 / 44 / 31 s); `--differential` clean 3,600-5,600 in four chunks and
600-1,000 (2,400 fields, 79-115 s a chunk). Windowed, paced, OpenGL,
3,600-4,800, quiet: 36.0 fields a second before, **54.4** after; p99 frame
94.5 to 56.1 ms; worst frame 100.9 to 63.1 ms; frames over 67 ms 104 to 0.
Headless OpenGL 59.0 fields a second at 135 s of process CPU for the run
(259 s before the helpers slept).

#### A new executable name, and the driver's own thread

Another round found the same Shrek 2 executable renamed `shrek_2.exe` (as
the hub names it) playing at 18 fields a second with OpenGL against 49 as
`G3RE52.exe`, and compiling programs for 17-22 s. Reproduced with the
final build in a folder of its own, launched as the hub launches it (no
arguments but the renderer; the cache beside the executable):

| run | name | our program cache | programs | fields/s (3,600-4,200, windowed) |
|---|---|---|---|---|
| 1 | `shrek_2.exe` | empty | 201 compiled, 42.9 s on the compile thread | 11.5 |
| 2 | `shrek_2.exe` | full | 201 loaded from disk, 14.3 s | 17.6 |
| 1 | `G3RE52.exe` | empty | 203 compiled, 3.2 s | 50.3 |
| 2 | `G3RE52.exe` | full | 203 loaded, 3.5 s | 51.0 |

Our cache works whatever the name: every program came from disk on the
second run. What differs is the driver. For an executable it has not seen
much, NVIDIA 616.64 runs our OpenGL calls on a thread of its own inside
`nvoglv64.dll` (92 % of a core, sampled per thread; our render thread 37 %)
- its threaded optimisation - and each EFB copy's `glReadPixels` into a
pixel buffer then waits for that thread: 86-140 ms a copy against 26 ms,
the render thread falls seconds behind, the 512 MB command queue fills and
the machine waits on it (`full_waits` 72-125 in 300 fields, frames of 2-6
s). With a known executable the driver's thread is gone and our render
thread does the work itself. The same unknown name headless, with every
program from our cache: 19.0, 28.8, then **57.7** fields a second on the
third run and every run after; overwriting the executable with a rebuilt
one kept the fast state. So the driver learns an executable over a few
runs and keeps what it learned through a rebuild, which is the hub's case:
the user's `shrek_2.exe` has run for hours.

Tried, none of which changed the unknown-executable runs: the driver's
compile threads capped at 4 or 2 (`GC_GL_COMPILE_THREADS`; 43 s either
way), `__GL_THREADED_OPTIMIZATIONS=0` in the environment (not read on
Windows), reading `GL_UNSIGNED_BYTE` instead of `8_8_8_8_REV` (the same
bytes), a pixel buffer made with `glBufferStorage` and
`GL_CLIENT_STORAGE_BIT` (state hash unchanged, speed unchanged), a
`glFinish` before each read, and the helper threads off. Bounding the
command queue at 64 MB instead of 512 MB took the first unknown run from
11.7 to 18.2 fields a second and its worst frame from 2.6 s to 1.0 s (16 MB:
0.58 s), but cost the warm case 1-2 % (52.2 against 51.1 fields a second
headless) and was not kept. Not done here, because it changes the user's
driver settings: an application profile with threaded optimisation off
(NVIDIA Control Panel, or NVAPI's settings database), which is what the
driver arrives at by itself after a few runs.

#### What is left

- The snail area costs 45-50 ms of machine-thread processor time a frame:
  about 33 ms of guest code and 14 ms of command stream, flushes and OpenGL
  batching. 59.94 there needs the guest code about twice as fast - an
  emitter that keeps registers in locals and checks for events per block,
  not per instruction - or the vertex work off the machine thread.
- Waking the helper threads costs the machine thread about 15 % in gameplay
  (`gc_gx_workers.c`, another round's file): a flush wakes them for about 74
  triangles.
- Windowed, paced loading (600-900) runs 53.3 fields a second while each
  field takes 16.0 ms running and 0.7 ms presenting: the frontend's clock no
  longer banks debt, so a field that runs over the 16.7 ms is not made up.
  The menu load's guest code needs to get well under the budget, not just
  under it.
- A hub build takes about four times as long with pointer roots; the
  per-instruction code size is the lever for that as well.
- Held pages and stepping are Windows-only, as holds are.

### Quick wins: the hub profiles play, and helpers that sleep (2026-09-15)

Two changes for Shrek 2 played from the hub (`feature/gc-quick-wins`).
The machine was shared with three other agents' Shrek runs and a player
throughout, so every figure below is a back-to-back pair taken when the
load before the run was low (shown); pairs spoiled by load arriving
mid-run were thrown away rather than averaged.

**The hub's profile plays.** The cover run (3,000 fields, no input) never
reaches play, so a hub build interpreted play's function-pointer code.
Hub policy is now: import, build (no trace), capture the cover (writes
`profile/cover.trace`), rebuild, run the backend's *profile runs* as a
`Profile` job (GameCube: `--frames 3000 --auto-input --trace-out
profile/play.trace.part`, renamed to `play.trace` only when the run exits
cleanly), rebuild. A capture or profile job that creates a trace file the
game did not have queues the rebuild, deferred while the game runs from
the hub; a profile job is queued after a build of a game that has a cover
and lacks a profile output, once a session a game, so a failing run cannot
loop. A failed run leaves the cover's trace alone and the build keeps it.

Why a separate job after the first rebuild rather than inside the
capture: the play run on the untraced build took 284 s (load 7 %); on
the build with the cover's trace 57 s (load 31 %). The first version ran
it inside the capture and the capture took 6m 21s.

| hub batch import, fresh data dir (loaded machine) | time | blocks |
|---|---|---|
| build, no trace | 31.4 s (compile 25.3 s) | 22,839 |
| cover capture, 3,000 fields | 37.9 s | |
| rebuild with the cover's trace | 40.1 s (compile 33.2 s) | 34,200 |
| profile: 3,000 fields of `--auto-input` | 3m 17s | |
| rebuild with both traces | 1m 07s (compile 59.6 s) | 55,059 |
| **total** | **6m 23s** | |

`--rebuild-all` with both traces at 13 % load: 53.9 s (compile 48.8 s).

Scripted play from a state at field 3,600 (`gc-gameplay/play.txt`),
headless:

| build | interpreted, fields 3,600-4,800 | native share | software, fields/s | OpenGL, fields/s |
|---|---|---|---|---|
| cover trace | 297,110,212 of 9.73 G | 96.95 % | 30.5, 30.6 | 42.3, 42.3 |
| cover + play traces | 863,734 | 99.991 % | 35.1, 35.3 | 48.8, 48.8 |

(fields 3,600-4,200, both builds on main's runtime, pairs at 8-18 %
load.) The hub's own build with both traces interprets exactly the same
863,734 and runs 36.8 like the manual one.

Found on the way, for the OpenGL work: the same executable renamed from
`G3RE52.exe` (the name every agent's runs use) to `shrek_2.exe` or any
other name ran OpenGL play at 18 fields/s instead of 49 - its 101
programs took 17-22 s to compile on the compile thread against 2.3 s, and
the render thread waited behind them (copy_ms 10.8 s against 0.3 s). A
second run under the new name was no faster. It looks like the driver's
shader cache is per executable and warm only for the name used for weeks;
the hub's games are named after the game, so a player's first OpenGL
runs pay it. Not investigated further.

**Helpers that spin only as far as the jobs pay** (`gc_gx_workers.c`).
Every helper spun 4 ms after every job, and jobs come a few hundred
microseconds apart with either renderer. Now the dispatcher measures the
share of its time inside jobs over 50 ms windows and lets helpers 1 to
count x share / 40 % spin (at least one); a helper still sleeps 4 ms
after its last job. Sleeping helpers are woken for a job only when that
function's work - time with helpers times the helpers that took a piece
plus one, averaged - is at least 1 ms. `GC_RASTER_SPIN_ALL=1` restores
the old behaviour and is the "before" below (same executable, merged
main, both traces):

| window | renderer | before: fields/s, cores | after: fields/s, cores |
|---|---|---|---|
| boot to 2,400 | software | 5.7 s wall, 15.5 | 5.7 s, 15.1 |
| boot to 2,400 | OpenGL | 5.9 s wall, 13.4 | 5.9 s, **3.2** |
| cutscene 2,400-3,000 | software | 53.1, 15.0 | 51.7, 14.9 |
| cutscene 2,400-3,000 | OpenGL | 62.5, 12.2 | 63.8, **7.4** |
| play 3,600-4,200 | software | 36.6-36.8, 13.3 | 36.8, 13.4 |
| play 3,600-4,200 | OpenGL | 50.4, 12.2 | 50.8, **6.0** |

Software play measures 40-50 % of the caller's time in jobs, so every
helper keeps spinning and nothing changes; OpenGL play measures 12-15 %
(triangle setup on `prepare_held`, 6-8 µs a job) and keeps five or six.
Cores are total process CPU time over wall time. The software cutscene
pairs were 53.1/52.6/53.1 against 51.7/53.1/53.1; two further runs of the
new code under 28-34 % load fell to 39: with fewer helpers spinning,
another process gets the cores the old code held.

Tried and dropped:
- Each helper spinning for four times its own average gap between jobs,
  and a per-function "helpers that took a piece" count choosing whom to
  wake: woken helpers arrive after a 6-50 µs job is done, so jobs ran 3-8
  times longer (`prepare_held` 6 µs to 50 µs, bands 44 to 140 µs).
- Every helper spinning only from an 80 % share: OpenGL play 4.6 cores at
  50.4 fields/s, but software play 31.7 against 36.6.
- With no helpers at all OpenGL play is 44.4 fields/s on 2.2 cores.

Gates, on the merged branch (main `88730ab`): `run_checks.py --quick`
with the disc and main's frames, everything passed (CTest 21 of 21,
recompiled tests agree, frames at the six fields identical, audio); the
traced build's frames at 600-3,700 identical in software and `--gl`, for
both a manual build and the hub's own; `--differential --auto-input`
clean 0-750 and 750-1,500 (from a saved state) on both, 99.993 % native;
the state at 4,800 scripted fields is byte-identical with and without
`GC_RASTER_SPIN_ALL`. (Merged main's pc at that field is `00000500`
where the branch before the merge had `802474c0`, with the same
instruction count - from the audio or card merge, not these changes.)

### The graphics thread (2026-09-15)

Branch `feature/gc-gx-thread`. The goal: take the command stream's drawing
off the game's thread, with nothing the game can observe moved by one
instruction. `runtimes/runtime-gc/src/gc_gx_thread.c` (the queue, the thread, the
shadow machine), `gc_gx.c` (`scan_stream`, the walk on the machine's thread,
and `gc_gx_exec_commands`), `gc_memory.c` (tracked pages), `gc_gx_raster.c`
(held triangles per drawing thread). `GC_GX_THREAD=0` keeps the
single-threaded path; the runner, `gc_run` and the frontend start the thread
otherwise (not a differential's shadow machine, which stays the reference).

#### What the guest can see, and where it is produced now

Read out of the code, not assumed:

| observable | modelled as | with the thread |
|---|---|---|
| CP read pointer, distance, status (idle bits) | `gx.read_ptr` moving over whole commands in `gc_gx_run`, put back when a command has not all arrived | the machine's walk moves it, with `run_stream`'s own control flow, in the same store |
| CP overflow/underflow flags, breakpoint, high/low water | plain register storage, not computed | unchanged |
| PE finish (BP 0x45 bit 1) and token (0x47, 0x48) interrupts, token value | raised in `write_bp` while the stream runs, inside the FIFO store | raised by the walk at the same place; the graphics thread skips them |
| EFB copy: bytes in main memory, `efb.copies`, the display buffer's address | `gc_efb_copy` in `write_bp` | prepared by the walk (counter, address), pages held (`gc_memory_hold_tagged`); made by the graphics thread, which releases the hold when the bytes are in memory. A read or write of a held page waits, as for the OpenGL path's copies |
| palette load (BP 0x65) | copies from main memory when written | the walk reads the entries; the queue carries them |
| matrix load from an array (CP 0x20-0x38) | reads main memory when parsed | the walk reads the values and queues a plain XF write |
| display lists | read from memory while parsed | the walk reads and copies them; the queue carries their commands inline |
| EFB peek / poke (0x08000000 window) | the framebuffer planes | wait for the graphics thread (`gc_gx_thread_target`), then as before; a peek on the OpenGL path now brings back one pixel of one plane (below) |
| perf counters, BP/XF read-back, bounding box | not modelled (plain storage or absent) | unchanged |
| counters (commands, primitives, vertices, triangles, lists, bytes, unknown commands) | parse counters | counted by the walk |
| transform and combiner registers, palettes, pixel count, unclipped triangles | the drawing's own state | the shadow's, copied back at every sync (`copy_back`) |

Nothing the processor reads changes when drawing happens; so drawing can
trail the processor as far as it likes, provided it draws from the bytes
the processor would have seen.

#### Memory: a copy of its own, fed in command order

Three designs were built and measured on the snail walk:

1. **Watched pages** (first commit). Pages a queued draw's arrays and
   textures lie on were made read-only with the burst's number; a write
   waited for the graphics thread to finish that burst. Correct (state
   hashes and frames equal), but the machine waited: 21,500 write faults
   and 6,300 waits in 300 fields, 2.2 s, most of them particle vertices a
   game rewrites between draws and heap data on a texture's first page.
2. **Array copies, sub-page pieces, stepped writes.** Arrays copied into the
   queue (a draw's index reach is a few hundred bytes: median 188); texture
   pages watched by the bytes they hold, writes beside them run one
   instruction at a time. Waits went, but a hot counter on a texture's first
   page took 2,000 single steps in 10 fields, and capping the steps brought
   the waits back (1,017 in 300 fields, all "not touching, steps used up").
3. **A copy of main memory for the graphics thread** (kept). The shadow's
   `mem1` is 24 MB of its own. Before each draw the walk queues the bytes the
   draw reads, read through the machine's view at that moment: the reach of
   each indexed array (min to max index, from the draw's own bytes), and
   each sampled texture's levels up to the highest the level of detail
   reaches (`gc_gx_texture_span`, the same bytes the OpenGL cache hashes).
   The pieces of pages a range shares with other data go every time; whole
   pages go only when written since last sent - a sent page is *tracked*
   (read-only; the first write opens it for good and forgets the mark;
   `gc_memory_track`). A hold forgets the marks of its pages, since a copy
   writes through the second view where nothing faults. The graphics thread
   applies the bytes in order; bytes equal to what it holds change nothing,
   different ones first draw whatever triangles it is still holding back.
   No write ever waits for the graphics thread.

On the snail walk the machine sends 1.7 MB a field in 1,650 ranges; 97 % of
it equals what the copy already holds (the price of not tracking pieces of
pages); 7 whole pages a field; 5 write faults a field.

Why it is exact: every byte the drawing reads from memory is either in the
queued commands (lists, palettes, matrices) or sent immediately before the
command that reads it, read at the moment the single-threaded path would
have read it (inside the same FIFO store, with the processor stopped). A
copy's destination is held from the command on; the walk's own reads of
memory wait for it; the copies are handed over at once, so the wait cannot
be on a copy nobody has. Triangles held back are drawn before any register
write, any copy, any palette, and any byte they could read changes.

#### Sync points, and how often Shrek 2 hits them

A sync (`gc_gx_thread_sync`/`target`) waits until the queue is drained and
the graphics thread has drawn what it held: framebuffer peek and poke, a
state saved, loaded or hashed, a differential comparison, the OpenGL path
started or stopped, the frontend's final hash. On play.txt 0-6,000 and the
walk: **no sync during play** (4 a run: start and the end-of-run hash);
Shrek 2 never peeks or pokes the framebuffer in these 6,000 fields (the
OpenGL path's `peeks=0`). EFB copies (one display copy a frame, plus render
to texture in the storybook) never sync; the graphics thread makes them and
the held pages carry the wait to whoever reads them.

Two bounds keep drawing close behind: the machine waits when the graphics
thread is more than two fields behind (`GC_GX_LAG`), and on the OpenGL path
the graphics thread waits once two copies are on their way to memory
(`GC_GL_COPY_LAG`; display copies only, since 2026-10-07 - "The waits that
kept three threads in step"). Without the second, the render thread fell a second
behind the card (copies landing 970 ms after they were asked for, the
512 MB command queue full 328 times); with it, 60-90 ms.

The copy bound is why OpenGL cutscene numbers below look like a loss (91
fields/s against 105 with the thread off). They are not: headless and
unthrottled, the single-threaded path also outruns the card, and its
copies reach memory 630-735 ms late on average (`mean_ms_to_memory` in
the stats), a backlog it builds and never pays back inside a 600-field
window. With the bound the copies land in 61 ms and the rate is the
card's. Alternatives measured on the cutscene and the walk: a bound of 4
or 8 copies (90 and 92 fields/s, same as 2), no copy bound but the field
bound (106 and 55.5, copies 990 ms late on the walk), no bounds at all
(105, worst frame 433-485 ms). Two copies stays.

The floating-point environment goes with the commands (the SSE modes,
queued when they change): a transform computed on the graphics thread uses
the rounding the machine's thread had.

#### A bug the gates did not catch at first

The frontend starts the thread at boot and then loads the state; the runner
loads first. `gc_gx_thread_reload` synced before handing the loaded state to
the shadow, and a sync copies the shadow's registers back over the
machine's - the combiner, transform and palette state of the moment before
the load. The frontend diverged in the first field after `--load-state`
(hash `6af5c904b2c3c3bb` against `1442123cb2349efd` at 3,700). Found by
comparing the frontend's hash with the thread on and off; fixed by draining
without bringing anything back.

And a second: the drain before a peek or poke sat inside
`gc_gl_fetch_pixel` and `gc_gl_invalidate`, which are empty in a build
without SDL2 - which is how the recompiled test programs are built. So
`gc_textest` peeked pixels the graphics thread had not drawn yet, and
failed differently in the recompiled and interpreted runs (the
`recompiled` stage of `run_checks.py --quick`; the frontend, runner and
Shrek gates all have SDL2 and never showed it). `gc_efb_read` and
`gc_efb_write` now drain themselves: `gc_textest` 112 syncs, no failures.

#### Peeks read one pixel

`gc_gl_fetch_pixel`: while memory's framebuffer is out of date, a peek brings
back that pixel of that plane (`glReadPixels` 1x1) into its place; past 64
in a field the whole framebuffer comes back once. `gc_textest` 111 peeks, 64
of them single pixels; `gxtest_copyfilter` 2,048 peeks with memory already
current. Shrek 2: none.

#### Vertex transform on the card: measured, not built

Sampling every thread of a snail-walk run (OpenGL, thread on): the
graphics thread is idle or waking helpers most of the time (38 % asleep, 31 %
inside the helper pool, 13 % `SetEvent`); decoding, transforming and
lighting are about 5 % of its samples; the machine's thread does none of it
any more. The card is at **98 % utilisation** (`nvidia-smi`, sampled every
200 ms) with the thread on and off, and the render thread spends its time
inside the driver waiting for it (`glReadPixels` into a pixel buffer 26 ms
a copy). Moving transform, texgen and lighting into the vertex shader would
move work from threads with room to the one part that has none, and the
CPU still needs every position for clipping, culling, bounding boxes and the
register carry. Not done.

#### Measurements

Runner (`G3RE52.exe --load-state ... --input ...`), headless and
unthrottled, back to back on a quiet machine (foreign load 4-11 %), main
`4ff8bd1` against the branch with the thread on and with
`GC_GX_THREAD=0`. Fields a second over the window (first ten fields
dropped); machine-thread CPU ms a field from the field log.

| scene | renderer | main | thread on | thread off | machine cpu main -> on | graphics thread cpu |
|---|---|---|---|---|---|---|
| menu load 600-900 | software | 58.2 | 56.3 | 56.2 | 16.9 -> 17.3 | 0.9 |
| menu load 600-900 | OpenGL | 58.3 | 57.2 | 57.7 | 17.0 -> 17.2 | 0.6 |
| cutscene 2,400-3,000 | software | 52.0 | **87.6** | 51.2 | 18.9 -> 7.1 | 11.2 |
| cutscene 2,400-3,000 | OpenGL | 91.4 | 90.5 | 105.2 | 9.1 -> 6.2 | 3.1 |
| snail walk 3,600-4,800 | software | 36.6 | **64.2** | 35.8 | 26.7 -> 14.5 | 13.6 |
| snail walk 3,600-4,800 | OpenGL | 56.6 | 59.2 | 56.6 | 16.9 -> 12.5 | 5.1 |

The menu load is the guest's own code (the machine thread's 17 ms are
blocks, not drawing). The OpenGL cutscene "off" figure is the backlog
described above (copies 735 ms late); main's is the same (633 ms).

Frontend, windowed and paced, snail walk 3,600-4,800 (`--play-input
walk.txt`), three runs of OpenGL and one of software:

| renderer | main | thread on | thread off |
|---|---|---|---|
| OpenGL fields/s | 53.5-53.6 | 54.6-55.0 | 53.3-53.5 |
| OpenGL worst frame (two fields) | 138-147 ms | 122-131 ms | 143-151 ms |
| OpenGL frames over 40 ms (of 595) | 146-150 | 99-119 | 150-164 |
| software fields/s | 37.9 | 58.7 | 38.5 |
| software worst frame / over 40 ms | 115 ms / 592 | 80 ms / 23 | 130 ms / 593 |

Syncs: 4 a run (start, the end-of-run hash) on play.txt 0-6,000 and on
the walk - none during play. Framebuffer peeks in Shrek 2: 0.
`gc_textest` 112 syncs (111 peeks), `gc_fogtest` 23.

120 fields a second was the goal and is not reached: the software path's
machine thread is now at 7 ms (cutscene) and 14.5 ms (walk) a field,
most of it guest code, and the OpenGL path is bound by the card.

#### Gates

All on this machine, thread on unless said.

- play.txt 0-6,000, frames every 300, `--state-hash`: main software,
  thread on/off in software and OpenGL, thread on twice more in each
  (625a537), and the final build on in software and OpenGL and off in
  OpenGL: every one field 6,000, 48,648,700,000 instructions, pc
  `800a2ec4`, hash `c686a58cae3aa5b7`, full `a6a9e6df951c399b`, all 20
  frames identical to main's (71-400 s a run depending on renderer and
  load).
- `--differential`, thread on, from saved states, 3,000 fields: 600-1,100
  (121 s), 2,400-2,900 (141 s), 3,600-4,100, 4,100-4,600, 4,600-5,100,
  5,100-5,600 (the snail walk; 191-346 s): the machines agree at the end
  of every field.
- Save state at 3,900 with the thread on, resumed to 4,200: hash
  `8239ec5181a9476c` / full `9dc78f3fab8b00b1`, as the straight run, in
  software and OpenGL. Frontend `--load-state` 3,600 -> 3,700 in
  software and OpenGL, thread on and off: `1442123cb2349efd` all four.
- `run_checks.py --quick --disc ... --frames-ref <main's frames>`
  (final build): everything passed in 209 s wall - CTest 21 of 21
  (hardware test baseline included), every recompiled program agrees
  with the interpreter, frames at the six fields identical, audio.
- `compare_gl.py --modes hybrid`: 16 of 16 agree (93.5 s).
- `gc_gl_fuzz` hybrid, 3,000 cases, seed 41 (901e080): 0 colour and 0
  depth differences.

#### Left open

- **The OpenGL path is bound by the card** in the swamp: 98 % GPU at
  50-55 fields a second. The fragment program computes in doubles for every
  pixel of every triangle grown by half a pixel (7.4 M box pixels a field),
  plus 355 destination copies a field. 120 fields a second there needs the
  card's work per field roughly halved - fewer fragments shaded (tighter
  coverage, earlier rejection) or cheaper arithmetic where it can be proved
  equal - not more CPU work moved.
- The software path is now bound by the graphics thread and its helpers.
- The walk costs about 1 ms a field on the machine's thread; sending bytes
  that did not change (97 %) is most of what could still go.
- One unthrottled software walk with the thread had a 407 ms worst frame
  (the paced frontend's worst was 80 ms); not chased.
- Windows only (tracking and holds need the second view); elsewhere the
  thread does not start.

#### A picture that stayed black (2026-09-15)

Reported from a play of the hub's build of `4c894a1`: skipping Shrek 2's
storybook and cutscenes made the picture wrong, in software. Branch
`feature/gc-skip-picture`.

**What the window did.** After A skipped the storybook the window stayed
black for 190-290 fields (the loading screen's last picture), then showed the
cutscene's first frame and kept it while the game went on into the level. On
`f657799` (the faster recompiler), paced, it never came back: from the skip
to the end of a 3,000-field run, and on the snail walk loaded from a state
not one of 1,200 presents showed a picture - in software *and* OpenGL (a real
window). Audio and the machine were right: the state hash is the same with
the thread on and off, `--differential` agrees over 1,300-2,800 across both
skips, and the headless runner's frames are the single-threaded ones.

**Why.** `present()` showed the buffer on screen only when no page of it was
held for a copy still on its way (`gc_efb_display_ready`), and otherwise kept
the picture before - with no limit. The field log of the frozen stretch: two
display buffers, `015a7fa0` and `0163dfc0`, and at every present the one the
video interface pointed at was held. The game copies a frame into a buffer
and points the video interface at it in the same field; while drawing trails
the machine (the graphics thread in software; the card's readback on OpenGL)
that copy has not landed when the field ends, so no present ever found its
buffer finished. A paced machine that finishes its field early makes it worse,
which is why the faster recompiler froze gameplay too. And the check was by
page: the two buffers share page `0163d`, so a copy into one made the other
look unfinished.

The gates did not see it because nothing compared the *window*: the runner's
frame dumps read memory, which waits for held pages, and the frontend's gates
compared state hashes.

**The fix** (`gc_sdl.cpp` `present`, `gc_efb.c`, `gc_memory.c`):

- The picture before stays up for one present at most
  (`PRESENTS_KEPT_MAX`); the next waits for the buffer on screen
  (`gc_efb_display_wait`). A paced present first spends the time the field
  would sleep anyway waiting for it.
- Finished-ness goes by the bytes pending copies write (the held byte
  ranges, as `gc_memory_view_for` already judged), not pages;
  `gc_efb_frame_rgba` reads a buffer that only shares a page with a copy
  through the second view, which does not wait.
- Tried and dropped: keeping two presents (and showing the buffer an
  earlier present could not, once finished). Unthrottled it waited less
  (33 waits against 185 over the skip); paced the waits were no better
  (3.3 s against 1.2-7.1 s for one present, on a shared machine); pictures
  were up to four fields late, and the earlier buffer was usable only 9 times
  in 1,500.

**Measured** (dummy video driver unless said; `f657799` merged, this
machine shared with other runs):

| run | before: presents kept / longest run | after: kept / longest / waits |
|---|---|---|
| skip script from boot, paced, software, 3,000 fields | 1,615 / 490 | 99 / 1 / 346 (4.0 s) |
| `run_checks` skip stage, unthrottled, software, 2,600 | 1,316 / 286 | 226 / 1 / 193 (2.9 s) |
| snail walk 3,600-4,800 from a state, paced, software | 1,200 / 1,200 | 13 / 1 / 600 (5.9 s) |
| snail walk, paced, OpenGL, real window | 1,200 / 1,200 | 600 / 1 / 599 (11.2 s) |

Paced speed is unchanged (60.0 fields a second from boot either way; the
walk 20.1 s both, fields ended late 54-81 against 109-131); unthrottled
software over the skip stage 121.9 against 130.0 fields a second.

**Regression check**: `run_checks.py --full` has a `skip` stage
(`tools/gc/scripts/shrek2_skip.txt`, `--shown-log`): the frontend with the
thread on and off; every present must show the thread-off picture of the
field its buffer is from, at most one field late, and the machines must end
in the same state. Before the fix: 1,547 of 2,600 presents wrong (longest
kept run 286); after: none. 46 s.

**Gates** (`f657799` merged): `run_checks.py --quick` everything passed
(150 s wall: CTest 22 of 22, recompiled programs agree, frames at the six
fields identical, audio); play.txt 0-6,000 frames every 300 and
`--state-hash` identical to `f657799`'s own build in software (58 s) and
OpenGL (72 s): hash `c686a58cae3aa5b7`, full `a6a9e6df951c399b`, 20 of 20
frames; `--differential` from a state at 1,300 through both skips to 2,800
(148 s + 154 s); the frontend's walk hash `ab3edd58fa1c3ad0` with the thread
on and off, before and after, software and OpenGL.

## The fast path (2026-10-06)

A second way to run the OpenGL path. It is the third renderer in a game's
menu ("OpenGL (fast)", kept in `gc_prefs.ini` as `display.renderer = 2`),
`--renderer fast` or `--gl-fast` on a game's command line, `--gl-fast` on
the headless runner and `gc_run`, "OpenGL, fast" under the hub's GameCube
renderer setting, `GcGlOptions.fast` in code and `GC_GL_FAST=1` in the
environment. Nothing selects it by default. It
gives up the byte-for-byte agreement the exact path is built for, in
exchange for speed. The exact path is unchanged and is still the one the
tests hold to the reference (`compare_gl.py`: 8 programs, three shader
modes, 24 of 24 agree after this change).

**What the card does itself on this path:**

- **Rasterises the triangle.** The vertex program reads the same 25-texel
  record and puts out the real vertices, with the vertex's own w so that
  the card's perspective-correct interpolation of colour and coordinate
  sets is the reference's, and depth stays linear across the screen. The
  triangle moves back a twelfth of a pixel so that the card's sample point
  (the pixel's centre) lands on the reference's (`px + 7/12`). Gradients
  for the level of detail are `dFdx`/`dFdy`.
- **Clips to the scissor**, as four clip distances from the record's box.
- **Tests depth early where the console does.** A pipeline with "depth
  before texture" set gets `early_fragment_tests`
  (ARB_shader_image_load_store), so a pixel the alpha test discards has
  still written its depth. It is a declaration and not a branch, so there
  are two general (uber) programs, one with it and one without ("Two
  general programs", below).
- **Blends.** No destination reads, so no layers: a batch is one draw
  however its triangles overlap. The program hands the card the source's
  term already multiplied and the destination's factor as a second output
  (`gc_gl_shader.c`, the last block of `main`).

The combiner, the texture unit, fog and the alpha test are still the
exact path's text.

**Speed** (Shrek 2, the dwarf cutscene from its save state, 857 fields,
headless, unthrottled, programs already compiled, RTX 5070 Ti):

| Renderer | Fields a second | Fields over 33.4 ms |
|---|---|---|
| Software | 63.6 | 178 |
| OpenGL, exact | 42.3 | 311 |
| OpenGL, fast: card rasterises | 80.3 | 135 |
| OpenGL, fast: card rasterises and blends | 126 | 2 to 4 |

Draws fell from 512,545 to 278,083 and layers from 329,145 to none.

Ordinary play from boot (4,000 auto-input fields, same conditions):
software 103.5 fields a second with 144 fields over 33.4 ms, exact 100.2
with 395, fast 182.4 with 2 (its 99th percentile is 18.5 ms).

**Picture.** On the test programs the last frame is identical to the
reference's (`gc_drawtest`, `gc_blendtest`, `gc_fogtest`, `gc_tevtest`,
`gc_textest`, `gc_spin`), and every verdict of those and of
`gxtest_bitfield`, `gxtest_clipping`, `gxtest_copyfilter` and
`gxtest_intensity` is the reference's.

**`gxtest_rasterization` fails 514 of its 519 subtests on this path**, as
the first OpenGL path did. It probes the sample point to a ten-thousandth
of a pixel (is a pixel whose sample is at 0.58330 covered, and at
0.58333?), and a card places vertices on a grid of 1/256 of a pixel. That
is the one thing this path gives up by design: which pixel an edge covers,
when the edge passes that close to a sample.

**The gate is `tools/gc/compare_gl.py --fast`**: each program through the
reference and through this path in every shader mode, the frame held to a
tolerance (at most 0.5 % of pixels more than 8 levels out, mean difference
at most 1 level; both are options) and the verdict lines held exactly,
except for `gxtest_rasterization`, whose verdicts it prints and does not
hold. 11 programs, three modes: 11 of 11 within the tolerance in each.

On the cutscene, eighteen frames fifty fields apart against the software
reference: 62 % of pixels differ at all, 0.035 % by more than 8 levels,
and the mean difference is 0.74 of a level. What is left is the six-bit
rounding and dither this path does not do, and edges.

**It did not start there, and the path found a fault in the reference.**
The first comparison was a mean of 4.9 levels apart with 27 % of pixels
more than 8 out, the fast path the brighter. The cause was two unmeasured
rules in the reference's output stage - a blend factor of "one" taken as
255/256, and a dither that eroded a six-bit pixel under every blended
layer - which made the scene's light shafts (about 187,000 large blended
triangles) several levels dark. Those are fixed in the reference
([The output stage](#the-output-stage-and-a-dither-that-should-not-fire),
2026-10-07), and this path's blend follows the same scale.

Two things tried for the card's blender and dropped. Taking half a level
off the source's term so that the card's rounding lands on the
reference's floor does nothing on an eight-bit buffer: the card rounds the
term to a whole level before it adds. And a sixteen-bit colour buffer,
where it would, bought no measurable agreement (mean 1.12 against 1.16 at
the time) for 2 % of the speed.

### Drawing larger than the console (2026-10-07)

The fast path can draw at two, three or four times the console's 640 by
528 each way: "Internal Resolution" in a game's menu under OpenGL (fast)
(`display.internal_scale` in `gc_prefs.ini`), `--internal-scale N` on a
game's command line, `--gl-scale N` on the headless runner and `gc_run`,
`GcGlOptions.scale`, `GC_GL_SCALE`. The exact path always draws at the
console's size.

**The machine never sees the larger framebuffer.** Drawing goes to one
`scale` times the size; everything that reads the framebuffer - a copy to
a texture or to the display buffer, a peek, a download for a save state or
a change of renderer - reads a console-sized one that the rows wanted are
brought down into first (`bring_down` in `gc_gl_render.c`), and an upload
is spread back out. Colour is averaged on the way down (the mean of four
at 2x; that twice over at 4x; at 3x one linear blit cannot weigh nine and
takes the middle pixel), depth takes one pixel of the block and never a
mean. Clears, the scissor's clip distances and the fog table's pixel
column are all still in the console's pixels.

**The display buffer is still the console's**, filled from the averaged
copy: that is what the video interface scans out and what a game can read
back. The window shows the larger picture by another way, below.

**A texture's level of detail follows the size drawn**, not the console's:
the gradients are the card's, per pixel drawn. That is the point - finer
levels where there are pixels to show them - and it is why `gc_textest`'s
mipmap test, which draws a texture a set number of the console's pixels
across and expects the level for that, reports a different level at a
scale. `compare_gl.py --fast --fast-scale N` prints that program's verdicts
at a scale and does not hold them; everything else is held as at 1x.

**The doubles had to go first.** At 4x the path ran the dwarf cutscene at
41 fields a second, a third of its speed at 1x: sixteen times the pixels,
each running the exact path's fog (a seventeen-term series in doubles for
`2^y`) and, for every tap of a texture that wraps, a remainder taken in
doubles. On the fast path both are single precision now, with the card's
`exp2`; the picture against the reference is unchanged to three places
(mean 0.742), and:

| Scale | Dwarf cutscene, fields a second | Fields over 33.4 ms (of 857) | Ordinary play, fields a second |
|---|---|---|---|
| 1x | 140.6 (128 before) | 0 | 191.4 |
| 2x | 140.5 (89 before) | 1 | - |
| 3x | 113.3 (59 before) | 5 | 160.3 |
| 4x | 84.9 (41 before) | 26 | - |

Headless, unthrottled, programs already compiled, RTX 5070 Ti; software
is 63 and 103.5 on the same two scenes.

**Against the reference at a scale** the cutscene differs more than at 1x,
as a supersampled picture should: at 2x, 6.3 % of pixels by more than 8
levels and a mean of 2.1, along edges and in texture detail (1x: 0.035 %
and 0.74). The test programs, which draw axis-aligned shapes, stay within
the 1x tolerance at every scale: `compare_gl.py --fast --fast-scale` 2, 3
and 4, 11 programs in three shader modes, 11 of 11 each.

`compare_gl.py` also refuses a program path that is not a file now: both
paths failed to start the same way on one, which read as agreement.

### The larger picture in the window (2026-10-07)

At a scale, each display copy also leaves the pixels it was made from, as
large as they were drawn. The render thread reads the copy's rectangle of
the drawing framebuffer into a buffer of its own beside the copy's rows;
the readback thread, which already waits for the copy, keeps it under the
place in main memory the copy wrote (`GcGlPicture`, three of them, because
a game shows one buffer while it draws the next). Whoever presents asks for
the picture of the buffer on screen (`gc_gl_display_picture`): the one
whose copy wrote where the video interface is reading, or none.

The frontend asks after it has read the display buffer itself, so the copy
has landed and its picture with it, and draws the larger texture in the
same place on screen (`show_larger` in `gc_sdl.cpp`). With no such picture
it shows the display buffer's, as before: a program that writes the
display buffer itself (`gc_spin`), a copy made without the readback thread,
a scale of 1. A loaded state forgets the pictures, since main memory is no
longer what they were pictures of.

What the larger picture is not: it has not been through the display
copy's vertical filter or gamma, nor the display buffer's shared chroma. A
write to the display buffer by the game itself after the copy is not seen.

**Measured** (Shrek 2 dwarf cutscene in a window, unthrottled, fields a
second): 1x 125; 2x 117; 3x 102 with the picture kept and shown, 108 with
`GC_GL_PICTURES=0`; 4x 79. A 3x run kept 438 pictures of 1920 by 1440 and
gave every one to the window.

**The gate** is in `compare_gl.py --fast --fast-scale N`: `gc_run
--dump-picture` writes the picture, which has to be exactly N times the
frame each way and, averaged back down, within 2 % of pixels more than 8
levels out and a mean of 1.5 of the reference's frame (worst seen: 0.94 %
and 0.69). 11 programs, three shader modes, scales 2 to 4: 11 of 11 each.
`--internal-shot FILE.bmp` writes the frontend's larger picture on exit;
`--screenshot-prefix` still writes the display buffer's, so a hub cover is
the same size whatever the setting.

**Not done yet:** the hub has a setting for the internal resolution ("GameCube
internal resolution", `hub/README.md`) and none for anisotropic filtering,
which is the game's own.

### The card filters the textures (2026-10-07)

The exact path keeps a texture as whole numbers and its program fetches
and filters texels itself, as the reference does: up to eight fetches and
the weights in integers for one trilinear sample. The fast path now uploads
the same bytes as fractions (`GL_RGBA8`) and asks the card, with the level
of detail still worked out the reference's way and handed over
(`textureLod`, in `sample_with_lod` under `GC_CARD_FILTER`).

A map's registers say how to filter when it magnifies and when it
minifies, and which it is doing is the program's decision, per pixel, from
that level of detail. A card decides it for itself from a level it works
out differently, so each map is bound twice, with a sampler object each:
unit k has the magnifying filter in both of the card's places, unit 16 + k
the minifying one, and the program reads the one its level says
(`apply_fast_samplers`, set only when a map's registers change). Wrapping
is the samplers' too. The one-texel-wide case the copies make (a texture
kept as two texels side by side) is still fetched by hand.

What can differ: the card weighs a bilinear sample with its own fraction
bits, and blends two levels with more of them than the reference's four.
On Shrek 2's cutscene against the software renderer the picture is where
it was (mean 0.74 of a level, 0.047 % of pixels more than 8 out).

**It buys about 2 % of speed, no more.** Interleaved runs of the cutscene,
one variable flipping the path (`GC_GL_CARD_FILTER=0` keeps the old one for
this comparison): 85.2 against 83.1 fields a second at 4x, 114.5 against
112.5 at 3x. The fetches were not what the fast path's time goes on. It is
kept for what it allows: filtering a card does that a program cannot do
cheaply, below, and textures that are not the game's own bytes, later.

**Anisotropic filtering** is the first of those. A floor seen at a slant
covers many texels along the view and few across it; the reference picks
its level from the longer run, and the floor blurs in both directions.
With the setting on, a minified, linearly filtered, mipmapped texture goes
to the card with its gradients (`textureGrad`, in `sample_bound` under
`GC_ANISOTROPY`), and the card takes up to 2, 4, 8 or 16 samples along the
longer run at the level the shorter one asks for. The map's bias goes in
as a scale on the gradients and its lowest and highest level as the
sampler's. Every other texture is filtered as before. It is off unless
asked for: Anisotropic Filtering in the game's menu under OpenGL (fast)
(`display.anisotropy`), `--anisotropy N` for one run, `--gl-anisotropy N`
on the headless runner and `gc_run`, `GcGlOptions.anisotropy`,
`GC_GL_ANISOTROPY`.

This is not the console's own anisotropic setting (two or four filter
cycles, bits 20:19 of a map's mode), which the reference still draws as
one (`docs/gc-mipmap-lod.md`, 3.6) and which nothing here reads.

**Measured.** `compare_gl.py --fast`, 12 programs, three shader modes: 12
of 12 at 1x and at 3x, and the same with `--fast-anisotropy 16`, whose
test textures are all seen face on and so should not move. (The twelfth
is `gc_timetest`, which the earlier runs at a scale left out. It draws
nothing, so it has a frame and no larger picture, and is now in the tool's
`NO_PICTURE` table beside `gc_spin`.) On a Shrek 2
swamp scene, the same frame with 16x and without: 0.18 % of pixels more
than 8 levels apart at 2x and 0.12 % at 3x, on tree trunks and hanging
moss. That is a small change, and it is the scene's doing: this game's
textures are small and mostly magnified, more so the larger it is drawn.
A game with a long textured floor is where it would show, and none has
been tried. 16x costs nothing that shows on this card: the cutscene at 3x,
interleaved, ran at 111.8, 115.2 and 115.3 fields a second without it and
114.1, 114.9 and 114.6 with.

One thing the frame comparison turned up, not about filtering: the
windowed frontend, unthrottled, from a saved state, does not always end
on the same game frame. Two runs of the same command can be one game
frame apart at field 31,200 (`--shown-log` shows the display buffer first
differing some forty fields after the load). The comparison above pairs
frames that match. It has not been looked into, and the headless runner
has not been checked for it.

### A game's frames in the checks (2026-10-07)

`run_checks.py` with a disc now has a `fast-frames` stage. After the
`frames` stage has run the game headless in software for 3,700 fields, it
runs it three more times with `--gl-fast`, at one, two and three times the
console's size, each from an empty program cache, and holds the frames at
fields 600, 1200, 2000, 2500, 3000 and 3700 to the software run's. The
measure is `compare_gl.py`'s: the share of pixels more than 8 levels out in
a channel, and the mean difference.

The limits are per scale, because the frame is a different thing at each.
At 1x it is what was drawn. At 3x it is the middle sample of each block of
nine, which lands where the 1x sample does. At 2x it is the mean of four,
so every edge and letter is smoothed: on Shrek 2's menu 84 % of the pixels
that are out lie on or beside an edge of the reference's frame.

| scale | worst share more than 8 out | worst mean | limit |
|---|---|---|---|
| 1x | 0.17 % | 1.39 | 0.5 %, 2.0 |
| 2x | 3.87 % | 2.02 | 6.0 %, 3.0 |
| 3x | 0.17 % | 1.39 | 0.5 %, 2.0 |

The mean is higher here than on the cutscene (0.74). The likely reason,
not checked: the menu and the logos are blended over the whole screen in
the six-bit format, which the reference rounds and dithers and the fast
path does not. Two software runs gave the same frames to the byte at all
six fields.

A run that does not say it drew on the fast path fails, as does a machine
with no OpenGL 4.1: the stage does not fall back to software. The three
runs took 13 to 16 seconds each by themselves with a warm program cache,
and 179 seconds together in a `--quick` run on a busy machine, beside the
test programs' compiles. At 1x, fields 600 and 2000 were the same to the
byte with the shaders specialised, general and hybrid, and from one
specialised run to the next.

### Two general programs (2026-10-07)

The general program reads a pipeline from a uniform block, so one text
draws everything. "Depth before texture" is the one setting it could not
read: `early_fragment_tests` is declared, not branched on. Until now the
fast path's general program left it out, so a pixel the alpha test killed
wrote no depth where the console would have: for as long as a pipeline's
own program was compiling in hybrid mode, and always with
`GC_GL_SHADERS=uber`.

The fast path now builds the general program twice, with the declaration
and without (`gc_gl_general_source`), and `general_for` in
`gc_gl_programs.c` gives a pipeline the one its shape asks for
(`shape[72]`). Both are built at the start in the uber and hybrid modes,
and kept in the program cache like any other. If the second will not
build, the first draws those pipelines as before and the log says so. The
exact path has one general program, as it had.

**The test** is new, in `gc_blendtest`: on each half of the screen a quad
every pixel of which the alpha test kills, then a quad a little further
away drawn only where nothing nearer was written. With depth before
texture the second is hidden; with it after, it shows. The software
renderer and the exact path pass it in all three modes. On the fast path
it passes with the shaders specialised either way; with them general or
hybrid it **fails without the second program** ("depth before texture:
got 200,60,90, wanted 40,80,120") and passes with it. Built for the Wii it
passes in software and on the fast path's general program.

**Cost:** with the driver's own cache defeated (`GC_GL_NONCE`) and ours off, the fast path's start compiles both in 186 to 200 ms on this card, over six starts, so the second is about 100 ms of a first start. From the program cache it is a load. (The exact path's one general program, a longer text, takes 361 to 391 ms.)

`gl: stats programs` has `uber_early_batches`: how many of `uber_batches`
the second program drew.

### The waits that kept three threads in step (2026-10-07)

Super Mario Galaxy 2 (`docs/WII-PLAN.md`) ran at half a console's speed
in its first scene on the fast path, and `--field-log` said none of it was
work: the machine's own thread used 9 to 15 ms of a 16.7 ms field and
spent as long again waiting. The game makes five copies of the framebuffer
to textures every field, draws with each at once, and reads a dozen pixels
back; Shrek 2, which this path was built on, makes one or two copies a
frame and peeks at nothing. Each wait was the machine catching the whole
pipeline up - graphics thread, render thread, card, readback - and five of
them a field is the pipeline run on one thread. Six changes, in the order
they were found, each of which uncovered the next:

- **A texture that is a copy still on its way is not read by the
  machine.** The machine sends the graphics thread the bytes each draw
  reads; for a texture those bytes were the copy's, not yet made, and the
  read waited on the held pages. Now the machine names the copy instead
  (`gc_gx_thread_send_texture`, by pieces: a texture's bytes run on past a
  one-level copy, often into the buffer the next copy is headed for), and
  the graphics thread, which made the copy, gets its bytes from whoever
  finishes it, taken before the pages are released
  (`gc_gx_thread_copy_landing`) so they are the copy's and not what the
  machine wrote next. Exact, and for every renderer: the software path's
  state hash is the same with it and without (`GC_GX_DEFER_COPIES=0`).
- **The fast path keeps the copy as a texture on the card.** Reading the
  framebuffer for the copy, the render thread also draws it into a texture
  of its own - the texels a sampler would decode from the bytes the copy is
  about to store, the copy's filter, intensity conversion and format
  narrowing done as whole numbers in a small program
  (`make_copy_texture`). A draw that names the copy's address, size and
  the format its bytes read back in takes that texture into its cache slot
  and needs no byte of it (`GC_GL_CMD_COPY_TEXTURE`). The bytes still go
  to main memory, behind. Only gamma (the card's `pow`) is not the
  reference's to the bit. `GC_GL_COPY_TEXTURES=0` turns it off.
- **A peek is answered from the frame before.** On the fast path a read of
  the framebuffer window no longer stops the machine until everything is
  drawn: each display copy leaves the whole plane a peek has been asking
  for (colour, depth) with the readback thread, and a peek is answered
  from the display copy before the last one issued
  (`gc_gl_peek_late`). Which frame answers depends only on what the
  machine has issued, so a run repeats. This is the one change that is
  not exact - a game sees the pixel one or two frames late - and it is the
  fast path's alone; `GC_GL_LATE_PEEKS=0` keeps every peek exact.
- **An access beside a pending copy is not an access to it.** The game's
  heap touches its block headers, which sit a few bytes either side of a
  copy's buffer on the same page, every field. The rule that lets such an
  access through assumed any instruction might reach eight bytes either way
  from where it faulted; it now works out how many bytes the instruction
  moves and where they start (`access_reach`, `may_step`).
- **The graphics thread waits for frames, not copies.** Its bound on how
  far it runs ahead of the readback (`GC_GL_COPY_LAG`, two) counted every
  copy; with six a field it was waiting all field long. It counts display
  copies now.
- **The second word of a pair of mails comes at once.** Not graphics at
  all: Nintendo's sound microcode answers in pairs, the game's handler
  reads the mailbox until the second word is there, and the second was
  being held back as long as a reply - 19,000 register reads a field, each
  one bringing the machine's clocks up to date. Sixty-four instructions
  now (`GC_DSP_FOLLOW_DELAY`). The title music is still Dolphin's to the
  bit.

Measured on Super Mario Galaxy 2's opening, the same script each time
(RTX 5070 Ti, the fast path at the console's size):

| | before | after |
|---|---|---|
| In a window, 6,000 fields | 74% of a console's speed (4,500 fields) | 96%; 59.7 to 59.9 fields a second everywhere but two loading stretches |
| Machine's thread a field, title | 18 ms | 8 ms |
| ... file select | 15 ms | 7 ms |
| ... the prologue | 31 to 35 ms | 14 ms |
| Waiting on held pages, whole run | 34.7 s | none |
| Peeks that stopped the machine | 29,876 | 73 (the first frames) |
| Mailbox reads a field | 18,900 | 180 |

What is left of the prologue's 14 ms is the processor itself, about 10,
and reading the command stream and sending the graphics thread its bytes,
about 4.5. Not done: the copies' bytes are still brought back for every
copy, used or not; and the floating-point handler's own blocks are run by
the interpreter because they turn the unit on in their second instruction
and are checked at their first.

`gx: thread stats` has `copies_deferred`, `copies_resolved`,
`copies_waited`; `gl: stats copy textures` and `gl: stats late peeks` are
new lines; `gl: stats batching` has `late_peeks` and `copy_textures`.

What holds it (Windows, 2026-10-07): `tests/gc/gc_memory_test.c` is new -
which copy a byte belongs to, the bytes each kind of host instruction moves
(39 hand-assembled), and real accesses a byte either side of a held range
and one byte into it. Shrek 2 through `tools/gc/run_checks.py`: the software
renderer's frames at all six fields and the machine's state hash after 3,700
fields are `main`'s, and the fast path's frames at one, two and three times
the console's size are inside the tolerances they had, now with 1,992 of its
textures taken from the card (`taken`, above). Nothing tests the graphics
thread's bookkeeping of copies or the copy program's arithmetic on their
own: Super Mario Galaxy 2's picture against the software renderer's (a mean
difference of 1.3 in 255) is the evidence for those, and a game is not a
test. One mistake there is worth knowing: a draw that named a copy the
table had already let go waited for pages a later copy in the same queue was
holding - a deadlock, and only in a window, where the machine runs at a
console's pace and copies land sooner. An entry is only let go with its
bytes in place, so there is nothing to wait for.

### A range's first and last page (2026-10-08)

Played by hand, Super Mario Galaxy 2 ran at nine tenths of its speed
whenever Mario moved (`docs/WII-PLAN.md`, "What the first hands-on play
found"): the machine's thread at 17 ms of a 16.7 ms field, none of it
waiting. A profile of that thread (`GC_GUEST_PROFILE=2000` with
`GC_GUEST_PROFILE_RAW=FILE`, read against the link map) had a quarter of it
feeding the graphics thread, and `gx: thread stats` said what with:
`bytes_sent` 8 MB a field, `bytes_unchanged` all but a thousandth of it.

The machine sends the graphics thread the bytes each draw reads, and a
page it has sent whole is tracked - read-only until written - and not sent
again. But only the pages a range *covers*. The partial pages at its two
ends were copied onto the queue every time a draw named them and compared
on the other side every time, and a model's arrays are a few kilobytes: for
most ranges the ends are all there is. Ten thousand ranges a field.

- **The partial pages are sent whole, once, and tracked** (`send_edge` in
  `gc_gx_thread.c`). Not when a copy is on its way to the page, whose bytes
  the send must not wait for; and not after the page has been sent that
  way six times (`EDGE_TRACK_LIMIT`), which is a page the game writes as
  often as it draws from it - there the piece goes every time, as before.
  A range whose pages are all tracked returns before anything else is
  looked at. `GC_GX_TRACK_EDGES=0` is the old way.
- **A draw's vertex size is kept with its format.** The scan worked the
  size out from the descriptor for every draw; it is in the table the
  format's indexed attributes were already in.

Measured on the game's prologue from a save state, 950 fields of Mario
running and jumping among the Toads (RTX 5070 Ti, the fast path at the
console's size):

| | before | partial pages tracked | and the size kept |
|---|---|---|---|
| Machine's thread a field, headless | 15.5 ms | 14.2 ms | 13.3 ms |
| Graphics thread's processor time a field | 13.6 ms | 12.6 ms | 12.2 ms |
| Bytes sent to the graphics thread, 1,500 fields | 9.58 GB | 45 MB | - |
| In a window: fields ended late, of 1,500 | 657 | 116 | - |
| ... times the sound ran dry | 7 | 0 | - |

The state hash after the 1,500 fields is the same in all three: what the
graphics thread draws from has the same bytes, sent fewer times.
`gx: thread stats` has `edge_pages` and `edge_pieces`; the field log's
`gl_wait_ms` now counts a late peek's wait for its frame, which it had left
out.

Tried and not kept: the scalar single-precision instructions (`fadds`,
`fsubs`, `fmuls`, the `fmadds` family) through the paired steady state's
kernel in recompiled code, with a note left for FPSCR as the paired forms
leave one. The state hash was the same and the handlers left the profile,
and the kernel call cost what they had: no difference in the field's time.

### A copy as large as the picture (2026-10-09)

An EFB copy kept on the card as a texture was the console's size whatever
the internal resolution: it was made from the console-sized planes the
copy's rows are brought down into for the bytes. So anything a game drew
*with* a copy of its own picture came back at the console's 640 pixels,
beside geometry drawn at two to four times that. Super Mario Galaxy 2's
prologue does it for its depth of field - everything further than the focus
is the picture copied to a texture and drawn back over itself, and the
focus follows Mario - and the owner saw it as "more blur" around Mario and
the Toads standing with him: blocks of four pixels, stair-stepped edges, a
sharp bush in front of them.

The kept texture is now made from the planes that were drawn into, at
`scale` times the copy's width and height (`make_copy_texture`, the copy
program's `p_scale`): the same sums, with the rows above and below for the
copy filter a console's row away. Nothing else moves. A draw samples a map
by fractions of it and works its level of detail out from the size the
registers give, so it does not know; the bytes in main memory are still the
console's, from the brought-down rows; at 1x the texture and every frame
are what they were. `GC_GL_COPY_SCALE=1` keeps the textures console-sized,
for comparing.

What it cost: a 4x copy of the whole picture is 22 MB of texture where it
was 1.4, made five times a field in that prologue. Making them takes 0.12
ms a field at 2x either way and 0.19 at 4x (0.14 console-sized); the
field's time did not move at either.

The checks' `fast-frames` limit for 3x is now 2x's. The 3x frame is the
middle sample of each block of nine and measured as 1x does - but Shrek 2's
menu reaches the screen through a copy, the copy is now three times as
fine, and the letters' edges land where 2x puts them:

| scale | worst share more than 8 out, console-sized copies | copies at the scale | limit |
|---|---|---|---|
| 1x | 0.17 % | 0.17 % | 0.5 % |
| 2x | 3.86 % | 4.23 % | 6.0 % |
| 3x | 0.17 % | 4.40 % | 6.0 % (was 0.5 %) |
| 4x | 3.81 % | 4.30 % | not in the checks |

Looked at, the 3x menu is the reference's with finer lettering. Not held by
a unit test: no test here has a graphics card.

### A copy found again by its bytes (2026-10-09)

Behind Shrek 2's pause book the scene went soft two frames after START, at
any internal resolution above 1x. That the backdrop is softer than play is
the game's doing: on START it copies the whole picture, halved, to a 320 by
240 texture and draws that back behind the book every frame, on a console
too. But the first two paused frames showed it from the copy kept on the
card - 960 by 720 at 3x - and every frame after from the bytes.

Each paused frame the game borrows the snapshot's buffer. Twice a frame it
copies the top left 256 by 256 of the framebuffer there to save it, draws a
page of the book in that corner, copies the page to a second texture and
draws the saved corner back; then its processor writes the snapshot back
from a copy of its own before the backdrop is drawn. The corner copy
overlaps the snapshot's entry in the graphics thread's table, which goes;
bytes from the processor are not the card's; so the backdrop was decoded
from main memory - although those bytes are the very ones the copy wrote,
and the texture the card made of them was still in the cache.

**The rule.** A texture named at a copy's address, of its size and format,
whose bytes are the bytes the copy wrote, is that copy, whatever the place
has held in between; the card's texture of it still stands for it. Equal
bytes are the whole test. Nothing about when the copy was made or what was
copied there since comes into it, because the picture a console would
decode from those bytes is the one the card's texture is a finer drawing
of.

- **What is remembered** (`copy_remember` in `gc_gx_thread.c`). When an
  entry goes - copied over in part or whole, pushed out of a full table - or
  bytes off the queue change what it wrote, and the card held its texture
  and a draw had used it, the entry moves to a second small table (eight)
  with the buffer its bytes landed in. One a place and shape, the newest.
  Nothing is copied or hashed to do it; an entry the card holds now keeps
  that buffer after its bytes are in place instead of freeing it.
- **Where it is asked** (`gc_gx_thread_copy_texture`). When no entry
  answers a draw, a remembered copy of that address, size and format is
  compared, byte for byte, with the graphics thread's memory - once, and not
  again until a byte there changes (a copy put in place over it, or bytes
  off the queue that differ: `memos_changed`, `copies_before_bytes`),
  however many draws ask. Bytes are compared and not hashed, so there is no
  collision to argue about, and a different picture is usually told apart
  in its first bytes. The answer is 2, not 1, and `map_key` in `gc_gl.c`
  uses the card's texture only if its cache still has the slot that took
  it; if not, the copy is forgotten and the texture is its bytes, as
  before.
- **Nothing depends on how far the card has got.** A copy still on its way
  when its entry goes is remembered too, by a buffer not yet filled, and
  whoever finishes the copy finds it there (`copy_landing_place`); a lookup
  waits for it as a read of its bytes would. Remembering only what had
  landed would have made the picture a matter of timing: in a run of Super
  Mario Galaxy 2 five copies in six went while still on their way.
- **What forgets.** A state loaded or the machine reset
  (`gc_gx_thread_reload`): afterwards the picture is what the state's bytes
  give, whatever the session drew before, so a state saved while paused
  comes back with the console-sized backdrop until the next pause. The
  OpenGL path stopped, which is also how the menu changes Renderer and
  Internal Resolution (`gc_gx_thread_copy_textures_gone`): what is
  remembered goes, and the entries that stand stop saying the card holds a
  texture - which they went on saying before, to a card that no longer had
  one. That was found by reading, not seen. And the cache having dropped the
  texture.
- **Where it is not.** Without a graphics thread, on the exact OpenGL path
  and in software no copy is ever held as a texture, so nothing is
  remembered and nothing is asked. `GC_GL_COPY_MEMO=0` remembers nothing,
  for comparing.

What it does not do: find a copy the game has moved to another address;
keep more than the newest copy of a place and shape (a game that takes a
second picture at the same place and then writes the first one back gets
its bytes); or survive a state load.

**The pause screen** (RTX 5070 Ti, from a state two fields before START,
which is pressed at field 31,500; the sharpness figure is the standard
deviation of a Laplacian over the top two fifths of the larger picture
brought to 640 by 480):

| scale | field 31,504 | field 31,520, before | with it |
|---|---|---|---|
| 2x | 16.81 | 7.24 | 15.62 |
| 3x | 19.48 | 8.26 | 18.85 |
| 4x | 21.75 | 7.24 | 21.20 |

The backdrop is 960 by 720 at 3x for as long as the book is open. It is
still softer than play, which the game asked for.

**What it costs**, counted, because nothing here could be timed while other
work shared the machine. A field, fast renderer at 3x:

| | remembered | bytes kept as they land | lookups | compared | found |
|---|---|---|---|---|---|
| Shrek 2, play (967 fields) | 0 | 0 | 0 | 0 | 0 |
| Shrek 2, paused (200 fields) | 1.9 | 0.12 MB | 1.4 | 0.9, of 0.13 MB at most | 0.5 |
| Shrek 2, start to the title menu (3,100 fields) | 0.3 | 0.02 MB | 0.13 | 0.07 | 29 in all |
| Super Mario Galaxy 2, 30,000 triangles a field (600 fields) | 3.1 | 0.58 MB | 0 | 0 | 0 |
| ... its pause screen (500 fields) | 3.0 | 0.64 MB | 0 | 0 | 0 |

Remembering is a buffer changing hands. "Bytes kept as they land" is the
one thing done that was not done before: a copy whose entry had gone before
it landed was handed to nobody, and is now copied into what is remembered
of it, on the thread that finished the copy - into the buffer of the copy it
replaces when that is the size, so a game that copies to the same places
every field allocates nothing. For Super Mario Galaxy 2 that is 0.6 MB
a field of `memcpy` beside the four megabytes a field that thread already
reads back from the card and converts (the figure moves with how far behind
the card is; the others are the same every run). The game never asks: every
copy it draws with still stands. Shrek 2's play makes no copies to textures
at all. The exit statistics have a line for it, `gx: stats copies
remembered` (`made`, `bytes_landed`, `lookups`, `compares`,
`bytes_compared`, `found`, `forgotten`), when anything was.

**What holds it** (Windows, 2026-10-09). `tests/gc/gc_gx_copies_test.c` is
new and is the first test of the graphics thread's bookkeeping of copies.
It has no graphics card, so it plays the card's part: it issues copies as
`gc_gx.c` does, lets the graphics thread make them the reference's way,
says which the card holds, and asks what `map_key` asks. Nine cases: a copy
stands for its texture and for no other size, format or address; the pause
screen's sequence, found only while every byte is the copy's and compared
once for a hundred draws; a second picture at the same place is the one
found afterwards; a copy covered whole before its bytes were put in place;
copies the card never held or no draw used; an entry pushed out of a full
table; more copies than are kept; forgetting (the cache, the renderer, a
loaded state); and the switch. Nine one-line mistakes put into
`gc_gx_thread.c` in turn each fail it. It cannot reach a copy remembered
while still on its way - without a card a copy lands as it is made - and
the games are the evidence for that part.

Shrek 2, the scratch build against a build of `main` (b3dba26):

- Every picture shown from two fields before START through a pause, play
  and a second pause (802 fields, 404 pictures): at 1x the same with it, with
  `GC_GL_COPY_MEMO=0` and on `main`; in software and on the exact OpenGL
  path the same as `main`; at 3x with the switch off, without a graphics
  thread, with `GC_GL_COPY_TEXTURES=0` and with `GC_GL_CARD_FILTER=0` the
  same as `main`. At 3x with it the pictures differ from `main`'s in the two
  paused stretches and nowhere else.
- With `GC_GL_COPY_SCALE=1` at 3x, where the card's texture of a copy is
  console-sized, all 404 pictures are `main`'s with it on: the remembered
  texture, used 325 times, is to the bit what the bytes decode to.
- Play to field 31,490 and the title menu at field 3,100, at 3x: `main`'s
  pictures, pixel for pixel. The menu's book rises over a snapshot of a
  black screen, found 29 times and the same black.
- Pause, play on, pause again: the second backdrop is the new scene (5.7
  levels from the software renderer's in the strip above the book, 27 from
  the first snapshot).
- F5 and F8 in a window while paused: the picture at field 31,950 is the
  one a fresh process shows there after loading that state, with the
  console-sized backdrop in both.
- Internal Resolution changed from 3x to 4x in the menu while paused: the
  4x picture afterwards (field 31,900) is `main`'s after the same clicks,
  and at the next pause (field 32,300) it is the picture of a run that was
  at 4x from the start, sharp backdrop included.

Super Mario Galaxy 2 at 3x, from the owner's "lag" state, 600 fields idle
and 500 through its pause screen: every picture the same with it and
without. (Its third and fourth pictures after a state load vary from run to
run, with the switch either way.)

Not done: a stage for this in `run_checks.py`; the checks above were run
by hand from a save state, which the gate does not have.

### A field on its own thread, and a present in two halves (2026-10-09)

The owner's next save state ("lag": Super Mario Galaxy 2 outside the
castle, 30,000 triangles a field) takes the machine's thread 16.0 ms of a
field's 16.7 at 1x. Three things were measured there.

**What the game had to spare.** The field log has a new last column,
`idle_instructions`: of the field's 12.16 million, those a wait loop
counted and did not run (`gc_idle_cycles`, `gc_machine_idle_until`).

| scene | machine's thread | idle |
|---|---|---|
| the prologue's start | 11.5 ms | 76.6 % |
| the Toads on the blocks | 13.2 ms | 66.7 % |
| "lag" | 15.6 ms | 57.7 % |

So the game is nowhere near its own budget - it runs 5.1 million
instructions in the heavy field and waits for the rest - and the machine's
thread spends 15.6 ms on those 5.1 million: 3 ns an instruction, of which
recompiled blocks are 54 %, floating-point helpers 16 %, walking the
command stream for the graphics thread 14 %. The profile across blocks is
flat (the hottest is 1 %): there is no routine to blame, only the cost of
every block.

**Tried and not kept: a block running its successor itself.** Every block
returns the next block to one loop that calls it, one indirect call from one
place to every block in turn, which looked like the cost. It is not. With
plain calls (`return next(m)`) the compiler does not always make the call a
jump and the stack overflows; with MSVC 14.50's `[[msvc::musttail]]` (C
needs `/std:clatest`) it refuses in every block that keeps a floating-point
register in a local, and in a few plain ones for no reason it gives
(C4737), which is no way to build other people's games; and bounded to
twelve calls deep by a counter, which is portable and worked, the field took
16.2 ms where it had taken 15.6. The same state hash each time. The
processor predicts the one indirect call well enough.

**The present was in the way.** The window's thread ran the field and then
presented it, one after the other, and a present is not free: 0.8 ms a
field at 1x, 1.5 at 2x, 2.5 at 3x (the larger picture is ten megabytes into
a texture). Sixteen and two and a half is nineteen.

- A field now runs on a thread of its own (`FieldRunner` in `gc_sdl.cpp`).
  The two threads never have the machine at once: the window's thread
  starts a field, does only the second half of a present, and waits for the
  field before it touches the machine again. Input, sound, save states and
  the menu are between fields, as they were. `GC_SDL_RUN_INLINE=1` runs the
  field on the window's thread as before.
- A present is two halves. `present_take` reads the machine - is the buffer
  on screen whole, the buffer into the console-sized texture, where it is -
  and `present_show` does the rest (the larger picture into its texture,
  the frame, the menu, the swap) and reads nothing of the machine while the
  game is not paused. `gc_gl_picture_at` fetches the larger picture by the
  place the first half read, without asking the machine and without waiting
  for the graphics thread's queue (`gc_gx_thread_drawn_on`); the first try
  used the function that waits, from the window's thread, while the machine
  was writing that queue, and the graphics thread found a record it did not
  know within a field.
- The second half is done at once when there is time before the next field
  is due (its own cost, averaged, and a millisecond), which is every field
  of a light scene: nothing about when a picture is shown changes there.
  When there is not, it is done while the next field runs - which starts at
  once, so the picture is on screen no later than it was.

The "lag" state, 600 fields in a window, paced, muted, fast renderer (RTX
5070 Ti); the same state hash with and without the thread at each scale:

| scale | | fields over 16.9 ms | debt written down | sound ran dry |
|---|---|---|---|---|
| 1x | on the window's thread | 118 | 32 times | 2 times |
| 1x | on its own | 105 | 3 | 0 |
| 2x | on the window's thread | 266 | 277 | 16 |
| 2x | on its own | 78 | 4 | 0 |
| 3x | on the window's thread | 532 | 581 | 38 |
| 3x | on its own | 465 | 469 | 36 |

**3x is the card's, not the processor's.** There the graphics card is 95 %
busy (`nvidia-smi`; 40 to 50 % at 1x), a copy takes 28 ms to reach memory
where it takes 7, and the machine waits 2.3 ms a field for frames it peeks
at. Not the larger picture's trip back to memory, which was the suspect:
with `GC_GL_PICTURES=0` nothing is read back and the numbers are the same.
The fast path's fragments are what a scale multiplies by its square, and
they are the next thing to look at; this change does not touch them.

## The clipper, and arithmetic that is not IEEE

Trivial rejection is unconditional: a primitive every one of whose vertices
is beyond the same plane is gone, whatever the clip-disable register says.
A primitive that *straddles* a plane is the one that register decides, and
when clipping is on it is cut against the two depth planes and fanned back
into triangles.

The valid range is **-w <= z <= 0**, inclusive at both ends. Every quad the
hardware tests draw sits exactly on the far plane, so a strict comparison
there draws nothing at all.

And the clipper does not use strict single precision. At a clip depth of
-1.0000001 against a w of 1 the exact sum is -1.19e-7 - negative, so an
IEEE clipper rejects the primitive - and the hardware draws it. One bit is
the smallest reduction consistent with that measurement, since the value is
exactly one unit in the last place below -1, so that is what is assumed
rather than a wider guess. How many bits the hardware really carries is
**not known**; what is measured is only that one unit in the last place
vanishes.

The other thing worth recording is what happens when a triangle has one
vertex beyond the far plane and the other two exactly on it. Cutting each
assembled triangle separately gives a polygon whose new vertices land on
top of the old ones - zero area, nothing drawn. That is what the hardware
does, and it is why a quad in that position disappears entirely rather than
losing a corner.

**`gxtest_clipping` is down to 4 of 16.** Those four are the steps the test
reads at a pixel its own geometry does not cover; each carries a TODO in
its source saying the read coordinate is provisional. Every step that is
internally consistent passes, including the two that need the non-IEEE
arithmetic.

## The framebuffer, and getting pixels out of it

The embedded framebuffer is 640x528 of colour and depth on the graphics
chip. It is not main memory and it is not laid out like memory: the CPU
reaches it through an address window where the pixel's coordinates *are*
the address - x at bits 2 to 11, y at bits 12 to 21, bit 22 choosing colour
or depth - so `runtimes/runtime-gc/src/gc_efb.c` models two planes of pixels and
decodes the address, rather than a block of bytes.

It also holds fewer bits than it looks like it does. The pixel format
register says how many, and a value written comes back rounded to that:
eight bits a channel with no alpha, six with alpha, or five-six-five, with
the top bits repeated into the bottom so that the largest value is 255 and
not 248. The rounding happens on the way in, because that is where the
hardware loses the bits.

Nothing reads the framebuffer directly except a peek. Everything else
leaves through a **copy**, and the order of its steps is load-bearing:

1. three rows are weighted and summed, clamped to the rectangle at its top
   and bottom edges if the trigger asks
2. the sum is shifted down by six and **masked to nine bits**, and only then
   clamped to 255 - so a large enough weight sum wraps round to a dark value
   instead of saturating to white
3. gamma, if it is not 1.0
4. luma and chroma, if both the intensity and auto-convert bits are set
5. tiled four pixels by four into main memory, a tile's alpha and red bytes
   in its first half and its green and blue in the second

Alpha goes through none of it: it comes straight from the row being copied.

The seven filter weights make three row sums, and not three-one-three: the
first two are the row above, the middle three the row itself, the last two
the row below. That is why libogc's default of 0, 0, 21, 22, 21, 0, 0 is an
exact pass-through rather than a blur, and it is the difference between
`gxtest_intensity` passing and failing on its last row.

**`gxtest_intensity` passes**: 262,144 assertions a block, every block, with
no rasteriser involved at all - the framebuffer is filled by pokes and the
whole test is the copy.

## The disc

`runtimes/runtime-gc/src/gc_disc.c` opens an image, reads its header and filesystem,
and serves reads at an offset; `gc_io.c` turns the DVD interface's four
command registers into those reads; and `gc_attach_disc` does what the
apploader does - the filesystem into memory, and the low-memory globals that
say where it went.

There is no retail disc in this repository and there never will be, so
`tools/gc/make_test_disc.py` writes a real one instead: the same header, the
same twelve-byte filesystem entries, the same 32-byte alignment the drive
reads in, with three files in it. It is built on demand rather than
committed, because a checked-in binary is a thing nobody can review.

Two tests, because neither half can see the other:

- `tests/gc/gc_disc_test.c` runs on the host, needs no devkitPPC, and covers
  the reader, the path lookup and the state a disc leaves in low memory.
- `tests/gc/programs/gc_disctest.c` runs on the machine and drives libogc's
  own DVD driver - `DVD_Mount`, `DVD_Inquiry`, `DVD_ReadPrio` - so the
  registers, the DMA and the interrupt are all exercised by a real driver
  rather than by a test written against our own model.

What the driver found:

| Behaviour | What it turned out to be |
|---|---|
| The DMA length register | It counts down. libogc's interrupt handler works out how much arrived as "asked for, minus what is left", so a drive that leaves the register alone says nothing moved - and libogc reissues the same read, forever. That was a hang, not a wrong answer. |
| The DMA address register | Walks forward with the transfer, and holds a physical address although software writes a cached effective one into it. |
| `dvddrvinfo` | Two halfwords and a word, not three words. |
| The low-memory globals | `gc_attach_disc` writes them, and a game reads them - but libogc is a loader in its own right and writes its own on the way up, so a libogc program cannot see them. The host test checks them instead. |

libogc's mount is more than a game's: it reads the disc id to decide whether
there is a modchip in the drive, and if there is not it starts patching the
drive's firmware. A read that works is what stops it - which is a fair test
of the drive, and worth knowing about before reading a trace of it.

## The estimate tables, measured

`fres` and `frsqrte` look their results up. The manual declines to print the
table, so `tests/gc/vectors/gen/gc_estimate_gen.c` measures it and
`tools/gc/fit_estimate_tables.py` fits it:

    segment = mantissa >> 47                      (32 of them)
    i       = (mantissa & ((1 << 47) - 1)) >> 37
    single mantissa = base[segment] - ((dec[segment] * i + round) >> shift)

with three tables - one for `fres`, one for each parity of `frsqrte`'s
exponent - and a rounding term that is measured too, because it is not the
same for both instructions. Every one of the 2,304 measured points comes
back out of the fitted tables exactly. That took `reciprocal` from failing
at its third subtest to 1 failure in 17,411, and `pairedmove` from 4,718
failures to 500.

## A recompiled game with a window, and the first picture

The frontend and the recompiler were built separately and never joined: the
generated project emitted its own headless `main` and linked only the
runtime, so the thing a person would actually play did not exist. It does
now. `gcrecomp` emits a `main` that prefers `gc_sdl_main` and falls back to
the headless runner - including when the arguments are the runner's own,
which is how the test tooling drives a generated game without a screen - and
a `CMakeLists.txt` that looks for SDL2 *before* it adds the runtime, because
that is what decides whether the window is built at all. `runtimes/runtime-gc`'s
Dear ImGui path had to stop being relative to `PROJECT_SOURCE_DIR`, which is
the generated directory in a generated project and the repository root in
this one.

The first launch of it crashed at once: `untranslated fetch at ea 00000000,
pc 00000000`. **The frontend was never setting the boot registers.** A
program that starts without a stack pointer runs until its first function
call and then jumps to address zero - which looks like a recompiler fault and
is not one. The runner had set them since P2 with a comment saying why; the
frontend was written from the runner and that part did not come with it.

The same launch exposed a second thing. The frontend loaded the *embedded*
image whenever a program carried one, and skipped the disc's own loader. A
disc has to boot through its loader even when the code is also embedded,
because the loader is what decides where the game's stack goes and therefore
where free memory begins: loading the image instead puts the right
instructions in the right places and hands the game a memory map it does not
recognise. The embedded image is for a program with no disc beside it.

**And then there was a picture.** Sixty seconds of game time - about seven
minutes of sitting there - and Shrek 2 draws its storybook intro: two curved
pages meeting at a spine, the crease down the middle, the cover's edge at the
left. 449,166 triangles, 5,412 display lists, 3,954 copies, 572 colours. The
geometry is right, so the transform, the clipper, the rasteriser and the
display-list path all work. The pages are white, and why they are white is
the next thing to find out.

**Not for the reason it first looked like, and it is worth writing down how
that went.** "White pages means the textures are not landing" was the first
thought, and the first measurement agreed: a probe counting what the sampler
returned said 5,925,489 texels out of 5,925,504 came back black, and the
addresses it read were mapped and full of zeroes. That is a very convincing
picture of a game whose texture upload never happens.

It was convincing, and it was an artefact of measuring one second into a game
that had not loaded anything yet. The same probe over the sixty seconds it
takes to reach the book:

```
TEXELS 1260678840 sampled: 387736131 white, 368642565 black, 504300144 coloured
```

Forty per cent of a billion texels come back with colour in them. **The
textures load and the sampler reads them.** Whatever makes that frame grey is
somewhere else - the combiner, the blend, or the frame simply being a moment
when the pages are blank - and the honest state of it is: not yet known.

The lesson is the ordinary one and it still caught me. The probe's window was
sixty fields because eight minutes was inconvenient, which is a reason about
the measurer and not about the thing being measured.

**What it actually was, and the machine said so out loud.** Running further
in printed a line that had been waiting since P3:

```
gx: an EFB copy this does not do yet: format 4, stride 64
```

A copy to memory is how a game makes a texture out of something it just
drew, and only the thirty-two-bit destination format was implemented -
everything else was deliberately deferred and *counted*, so that a game
leaning on one would be visible rather than silently blank. Shrek 2 renders
its storybook page into a sixteen-bit texture and draws the book with it, so
the pages came out as whatever was in that memory beforehand.

The other twelve formats are implemented now, tiled the way the sampler
expects to find them: 4x4 tiles for the sixteen-bit ones, 8x4 for the
eight-bit ones, 8x8 two-to-a-byte for the four-bit one. 572 colours became
3045, and the book acquired brown leather and a green illustration panel.

**And then a second bug the first one had been hiding.** Only the top right
of the book was textured. The pitch register counts thirty-two byte units,
and a tile *is* thirty-two bytes in every format except the thirty-two-bit
one, where a 4x4 block takes two of them - the alphas and reds in the first,
the greens and blues in the second. The halving that is right for that one
format had been applied to all of them, so every row of tiles landed at half
the pitch it should and the picture folded over itself. Right for the only
format that existed; wrong the moment the others did.

3102 colours, and the whole book. All 26 recorded programs still match, which
is the check that matters for a change to a path every one of them uses.

**A half-width copy.** Tracing every copy of one late field showed six, and
one of them made no sense: a 640x480 rectangle into a destination whose pitch
register said 80 tiles, which is 320 pixels. Its trigger differed from the
others in exactly one bit, bit 9, and that bit halves the width on the way
out. A game copying the whole screen into a texture it will only ever draw
small does not want it at full size. Ignoring the bit writes twice as many
pixels into each row as belong there; every row overruns into the next. It
is implemented now, averaging the two source pixels - **Unverified**: point
sampling is the other reading, and the two differ as shimmer on a moving
texture rather than on a still frame.

That fix is right and it did not change the frame at all, which is worth
saying plainly. There is still a flat brown square in the top left of the
book, and it is **not yet explained**. The first guess - content a
render-to-texture pass left in the framebuffer - was tested by wiping the
framebuffer after every texture copy, and the whole frame went blank. So the
scene is drawn *before* those copies and the square is part of it. Without a
reference image of that exact moment, further guessing about whether it is a
bug or the art is not worth much; a Dolphin capture synchronised to the same
point is what would settle it.

## Every texture format, and a test that could not fail

There is no hardware test for texture sampling. dolphin-emu/hwtests never
samples a texture at all - its TEV program turns every stage's texture off -
so the texture unit's evidence is `docs/gc-texture-formats.md`, compiled from
four permissively licensed decoders, and `gc_textest`, which checks the
runtime against the document.

`gc_textest` checked five of the eleven formats. The six it did not were I4,
IA4, RGB5A3, C4, C14X2 and CMPR - **and Shrek 2 samples RGB5A3 and CMPR more
than anything else.** All six are checked now, every expectation written from
the document and not from the decoder, since a test that copied the code
under test would agree with it about any bug it has. RGB5A3 is checked in
both of its layouts, because a decoder that reads five-bit fields in both
gets the colour wrong only on the transparent half. C14X2 is built with its
top two bits deliberately *set*, which is the check that they are ignored.
CMPR is checked in both modes, with endpoints chosen so that the GameCube's
`(5*C0 + 3*C1) >> 3` gives 159 and 95 where a PC decoder's exact thirds give
170 and 85.

All fifteen checks passed on the first run, and **a first-run pass is the
result most worth distrusting.** The check reads eight fixed texels, and with
the block indices laid out as `row + col` those eight never landed on index 3
in the four-colour mode or on index 2 in the other. Two of the formulas the
test existed for were never reached. The indices are now `col + 2*row`, which
puts the eight probes on every index, and that claim is computed rather than
asserted:

```
before: four-colour probes reach [0, 1, 2]     three-colour reach [0, 1]
after:  four-colour probes reach [0, 1, 2, 3]  three-colour reach [0, 1, 2]
```

**And then the test was made to fail on purpose.** Index 3's weights in the
runtime were replaced with exact thirds, and:

```
FAIL texel 5,3: got 85,0,159, wanted 95,0,159
```

at (5,3), which is precisely the probe the coverage fix added. Before it, the
same mutation would have passed. The runtime was restored and all fifteen
pass again.

One thing the mutation run nearly got wrong is worth recording. The first
attempt rebuilt `gc_run`, the link failed because a long hardware test was
still running on that executable, and the old binary reported a pass - which
would have read as "the test does not catch this". `gc_play` links the same
runtime and falls through to the runner for headless arguments, so it is
the one to rebuild while `gc_run` is busy.

What this still does not establish: that the document is right about the
hardware. It is a check of the decoder against four decoders' consensus.
The CMPR index-3 colour in the transparent mode is still disputed between
those sources, and the test deliberately does not assert it.

## Lighting, and a hardware test that had never been run

The storybook frame looks under-lit, and the honest answer to "is our
lighting right?" was: nobody knew. `gxtest_lighting` was on the runner's list
of programs too slow to run, and so it had never been run at all.

It is a 256x256 sweep, 65,536 cases, one copy each, with its verdict printed
only at the end. At the machine's speed that is 3,793 fields - the arithmetic
predicted about 3,790 - and about eight minutes:

```
Test 1 passed (65536 subtests)
```

Every case. **What that establishes is narrower than "lighting is right", and
the difference matters.** The test sweeps every material colour against every
ambient colour and checks how the lit result is rounded - the hardware does
this in fixed point with shortcuts, so the rounding is not obvious and is
exactly the thing worth measuring. It does not put a light in the scene. It
says nothing about directional or spot lights, attenuation, specular, or
normals. The core multiply is hardware-exact; the rest of lighting is not
covered by any test here.

`gxtest_copyfilter` came off the same list and passes all 6,539,264 of its
subtests. It is in the recorded baseline now, sampled at 120 fields the way
the intensity test already is. The lighting test stays off the default run -
it cannot be sampled, because its only verdict is its last line - and should
be run with `--slow` after anything touches lighting.

**Speed, measured fairly.** Both under SDL's dummy drivers, sixty seconds of
wall clock, the same disc:

| | fields a second | of the console's rate |
|---|---|---|
| interpreted (`gc_play`) | 4.8 | 8 % |
| recompiled (`G3RE52.exe`) | 6.5 | 11 % |
| recompiled, after the 2026-09-13 speed work | 24.9 | 41 % |

1.36x at the time, which agrees with the 1.43x the differential harness
measured on CPU-bound work (but see "Faster, measured on a game": that
figure does not reproduce, and the last row is a 1,800-field headless run
rather than sixty seconds of wall clock). The earlier figure of 10.6 from the same executable was
taken while it was crashed and executing nothing, which is the sort of number
that flatters a recompiler and means nothing.

## Open threads at the stopping point (2026-09-12)

Recorded here so the next session starts from what was measured rather than
from memory. Each has a first step that is already known.

**The normal-matrix address - fixed, pending hardware.** The runtime read the
normal matrix at `0x400 + 9*(index/3)`; libogc's formula and a second
implementation's software renderer both say `0x400 + 3*index`. The two agree
on every multiple of three - every `GX_PNMTX` a program names - so no test
here could tell them apart, and it is changed to the two-source answer.
`docs/gc-lighting.md` case F.18 would settle it on hardware. It does not
affect the storybook, whose lit vertices all use index 0 (measured).

**The DreamWorks intro - fixed (2026-09-13), and not for the reason
recorded here first.** See "The intro video, and a coordinate that was
already in texels" below. The guess written at the stopping point - that the
game decodes straight into the display buffer and our screen reader only
followed GPU copies - was half right: the reader did ignore the video
interface's registers, and now reads them, but that changed no frame of the
intro. The intro is drawn through the GPU and the sampler was reading one
texel of it.

**The title screen - fixed (2026-09-13) by the same change as the intro.**
Each surface was one flat colour, and the recorded hypothesis was the
smallest mip level. It was not: the game draws its title with texel
coordinates and a manual coordinate scale too, so every surface read one
texel. With the scale registers read, the storybook has its parchment,
emblem art, logo and menu text, and the company logos and legal screen
before it draw correctly (checked at fields 2100-4200).

**An earlier conclusion that was wrong.** The dark Dolphin book at its frame
1700 was compared with our lit one and read as "our lighting is flat". The
real title book is fully lit; the dark frame was an earlier moment in the
opening. The two cheap explanations tried along the way were both ruled out
by measurement: every lit vertex carries a normal, lights and diffuse, and
every one uses matrix index 0.

**Three lighting divergences from `docs/gc-lighting.md`, not yet acted on.**
In cases F.8 (spot cosine clamped at zero), F.13 (specular gated when the
light is behind) and F.14 (N.H clamped at zero), Dolphin's GPU and software
renderers agree with each other and this runtime is the odd one out. Each is
a missing clamp at zero. They are strong candidates to implement, but they
are not the explanation for the title screen and should not be claimed as
one.

**The TEV combiner hardware test has still never produced a verdict.** At
0.84 fields a second it ran its full 7,000-field budget - about two and a
half hours - without reaching the end of its first test. It needs a larger
budget or a faster machine.

**The flat square** in the top left of the storybook frame is gone with the
coordinate-scale fix - most likely the page's corner art read as one texel, though that was not traced.

## The intro video, and a coordinate that was already in texels

Shrek 2 opens with the DreamWorks logo, played from `/intro.lmj` (magic
`LXMX`: 512x384 frames, 22,050 Hz audio). It came out black: every field
solid black but for a two-by-two block in the top left corner whose colours
changed. It plays now. How that was found, because the obvious story was
wrong twice:

1. **The display buffer.** The recorded guess was that the game decodes into
   the display buffer without a GPU copy. The frame reader was changed to
   take the displayed buffer from the video interface's registers
   (`gc_vi_display`), which is right on its own terms - but every intro
   frame was still black. A trace of those registers shows the game flipping
   between two 640x480 buffers, both written by GPU copies.
2. **The decoder.** A probe on the triangles showed one full-screen quad a
   frame, textured with a 512x384 compressed texture, plus a row of small
   ones. A watch on that texture's memory found a decompressor writing it,
   and a probe on the drive found the game streaming `intro.lmj` a megabyte
   at a time, every 36 fields. The textures it produced were 72 % non-zero
   bytes, rotating through a ring of buffers. The video was decoding.
3. **The sampler.** The quad's texture coordinates ran from 0 to 512 and 0 to
   384 - texels, not the nought-to-one range the sampler assumed - and the
   sampler multiplied them by the texture's size again. Every pixel clamped
   to the bottom-right texel, which is black, except where the coordinate was
   below one: the two-by-two corner.

**What the hardware does.** The transform's coordinates are not normalised to
any texture. The rasteriser scales each coordinate by its own pair of size
registers - BP `0x30 + 2n` for s and `0x31 + 2n` for t, sixteen bits of size
minus one - and the sampler works in texels. libogc writes the bound
texture's size there before a draw, which is why reading the texture's own
size had been indistinguishable from reading the registers. A game that
calls `GX_SetTexCoordScaleManually` with a scale of one, and hands over texel
coordinates, is exactly the case where the two readings part - and a video
player is the natural thing to do that.

`gc_gx_texcoord_scale` is that step: the TEV stages, the indirect lookup and
the OpenGL shader all go through it. Frames from `gc_drawtest`, `gc_fogtest`,
`gc_tevtest`, `gc_textest` and `gc_spin` are byte-identical before and after,
which is what "libogc sets the scale to the texture's size" predicts.

**Unverified:** that the gradient a level of detail is picked from is scaled
the same way. It is here, which is what "the sampler works in texels" means
for a derivative, and it is untested - the intro texture has no mipmaps.

Tools added for it: `gc_run --dump-frame-every N`.

## The floating-point unit switched off (2026-10-07)

Found by the first retail Wii game (`docs/WII-PLAN.md`), and a fault of
the whole family: **the processor never took the floating-point
unavailable exception.** `MSR[FP]` was cleared on every exception, as it
should be, and then nothing looked at it. A floating-point instruction ran
whether the unit was on or off.

An operating system depends on that exception. Nintendo's saves and loads
the floating-point registers only when somebody wants them: an interrupt,
or a switch to another thread, leaves them where they are with the unit
off, and the first floating-point instruction afterwards traps to the
handler that puts the last owner's registers away and brings this thread's
back. Without the trap every thread and every interrupt handler computes
in the same thirty-two registers. Mostly nothing shows, because a thread
is rarely interrupted between loading a number and using it by something
that also does arithmetic. Super Mario Galaxy 2's sound thread does a
great deal of it. About a minute into a run its main thread, part-way
through turning a matrix into a quaternion, compared numbers that were no
longer its own, took a branch its own numbers could not have taken,
divided by nothing, and in one field the camera and then the whole scene
were NaNs. The moment moved whenever anything else changed the timing,
which is what made earlier failures of the same game look like different
bugs.

What is written:

- `gc_insn_needs_fp` (`gc_cpu_ops.h`): which instructions need the unit,
  by encoding - the floating-point loads and stores, primary opcodes 59
  and 63, and the paired singles (all of opcode 4 but `dcbz_l`).
- The interpreter (`gc_cpu_step`) raises vector 0x800 for one of those
  with the unit off, with SRR0 at the instruction, which then runs when the
  handler returns.
- Recompiled code: a generated function that holds such an instruction
  checks `MSR[FP]` where it is entered and, with the unit off, hands the
  block to the interpreter (`gc_rt_enter_stepped`), which raises the
  exception. An `mtmsr` inside a block that leaves the unit off sends the
  rest of the block the same way. One compare and branch for a function
  that uses floating point; nothing for one that does not.
- `gc_cpu_units` holds eleven instruction forms to it (the vector, SRR0,
  SRR1, nothing written), and that integer work and `dcbz_l` are let
  through. `gc_recompiled_fuzz` now runs one case in eight with the unit
  off, recompiled against interpreted; 1,901 of its 2,000 blocks carry the
  check.

**A project generated before this has no check in its blocks** and has to
be generated again; until it is, it behaves as it did.

What it changed for the GameCube, measured: Shrek 2, generated afresh and
run for 1,800 `--auto-input` fields beside the build against `main`. The
frames at fields 300, 600, 900, 1,200 and 1,500 are identical; at 1,800,
in play, nine pixels differ by at most 3 of 255. The state hash differs,
as it must: the exception's handler now runs. Which of the two pictures at
1,800 is the right one is not known - that is the frame where two threads
had most chance to have shared a register.

Tools this left behind, in the runner: `--watch ADDRESS` (every write to
an address, with the instruction), and `--trace-invalid FIELD` (from that
field on, every floating-point invalid operation - where a NaN is born -
and every store of a default NaN to memory outside the stack). The second
is what found the routine, in two runs.

## Where P5 stands

**All four gate items are met (2026-09-13).** The last one, the windowed run,
was met by making the machine faster - nothing in the audio path changed.

- **The saves work.** `gc_cardtest` drives the save library end to end and
  unmounts and remounts before reading back; `tests/gc/gc_card_restart.py`
  runs it twice against one image and insists the second run finds what the
  first wrote. The image on disk was checked from outside: a directory entry,
  an allocation table that agrees with itself, and the payload byte for byte.
- **The sound path's own tests pass.** The audio DMA raises its interrupt, a
  hundred buffers arrive in half a second, and the remaining-bytes register
  moves and stays inside the buffer.
- **The overlay menu opens from the keyboard and from a controller alone,
  pauses the game, and its settings survive a restart.**
  `tools/gc/menu_selftest.py` drives Escape, Guide and L3 through SDL's own
  event queue and checks the field counter stops.
- **A 30-second windowed run reports audio underruns**, and will until the
  machine is faster. The measurement: 234 fields in 30.1 s, 7.8 a second,
  13 % of the console's rate - so the sound device plays 130 ms of audio in
  the time the machine produces 17 ms. Nothing in the audio path fixes that.
  **Update 2026-09-13:** recompiled Shrek 2 now runs at 24.9 fields a second
  headless, 41 % of the console's rate (see "Faster, measured on a game"),
  still short of what the gate needs; the windowed 30-second run has not been
  repeated. A second round took it to about 34 fields a second, 57 %
  ("Faster again: the pixel path").
  **Met 2026-09-13:** a 30-second windowed run of recompiled Shrek 2 (real
  window, WASAPI, `--mute --audio-stats`) ran 1,798 fields, 59.9 a second,
  **with the queue never running dry** (mean 61.1 ms, minimum 39.9 ms);
  main managed 1,673 fields and ran dry 262 times the same morning. A
  90-second run is not yet clean - 4,752 fields, 52.8 a second, 589
  underruns - because of a loading screen that is all the game's own
  floating-point code and, past field 4,650, title-screen code the
  recompiler did not find. See "Full speed in a window: the P5 run".

There is no sound yet either: the engine plays whatever is in the buffer and
nothing fills it. That is `docs/gc-dsp-ax.md` parts B and C - walking the
command list, the voices, ADPCM - and it is the next piece of P5 rather than
a gate item.

## What the machine does so far

| Part | State |
|---|---|
| Address translation | The BAT walk, in both directions, with real mode when translation is off. No mapping is assumed: a game that sets its own BATs works. |
| Physical map | 24 MB of main memory, the 2 MB embedded framebuffer, the register block, and the locked cache at its own effective address with `dcbz_l` and the DMA that fills it. |
| I/O register file | Ten blocks described as data - offset, width, name, masks, and a hook where behaviour is needed. An access of any width lands on the right register, including the 16-bit halves of a 32-bit one. Anything undescribed still works but is counted and reported. |
| Interrupts | The processor interface's cause and mask, as levels rather than pulses, plus the decrementer. A pulse looks right until the handler acknowledges the device and the machine spends the rest of its life in the handler. |
| DSP | Enough to boot: the ROM's mailbox handshake, the microcode-upload protocol, the init microcode's reply, and ARAM DMA that really copies. Sound is P5. |
| EXI | Transfers complete, which is what the SDK's settings code waits on. Devices - the memory card, the real clock - are P5. |
| Video | Field timing, the beam position a program polls, and the four display interrupts. Nothing is drawn: that is P3. |
| Loaders | ELF with symbols, and DOL. |
| Hooks | `OSReport`, `__write_console` and `SYS_ResetSystem`, resolved from an ELF's symbols. This is how a test program reports a verdict; a game has no symbols and is never hooked. |

## Behaviour pinned by the vectors

Everything below is now fixed by a measured case, and any rewrite - the
emitter included - has to keep it. The list above says what each one is.

Still open, because the oracle carries no information about it:

| Question | What this core does | How it gets settled |
|---|---|---|
| `divw` by zero, and `0x80000000 / -1` | Result `0xffffffff` for a negative dividend, `0` otherwise; OV in the OE form | The vectors agree with this, but the manual calls it undefined, so hardware could differ. `hwtests/cputest`. |
| The `fres` and `frsqrte` estimate tables | Exact reciprocal, rounded | `tools/gc/gen_estimate_tables.py` has to capture all 32 and 16 entries from an oracle, which needs a generator program of its own. |
| `FPSCR[FI]`, `[FR]`, `[XX]`, `[OX]`, `[UX]` | Modelled from the manual through the host's exception flags | `hwtests/cputest/fprf.cpp` and the timing programs in P2. |
| Unaligned `lwarx`, `stwcx.`, `lmw`, `lswi` | Not implemented as alignment exceptions | Not measurable here: those instructions are outside the vector sweep (see `tools/gc/gen_vector_cases.py`). P2. |

## Known deviations

| Deviation | Why | Effect |
|---|---|---|
| `ps_res` and `ps_rsqrte` still differ on operands too small to survive truncation into the ps1 lane | The hardware does something the test's own author describes as "for some reason": a value that truncates to zero there comes back with its sign set, so the estimate of that lane answers as though the operand were negative. It is not clear yet whether that belongs to the truncation or to the estimate. | 500 of `pairedmove`'s 8,800 subtests, and 1 of `reciprocal`'s 17,411. Both are denormal corners; ordinary values pass. |
| `addme` and `subfme` carry | Both add three things: the operand, CA and `0xffffffff`. The oracle folds the last two together first and loses the carry out of `0xffffffff + 1`, so it reports no carry whenever CA was set. The architecture says both always carry in that case, and this core follows the architecture. | Listed with the estimates above; `hwtests/cputest` settles it. |
| Instruction timing is one cycle each | P8 owns timing, and measuring it needs the machine model first | The timebase advances, but at a rate no game should be trusted to. |

## Risks

1. **Flipper is the project.** P3 is larger than the whole SNES backend. If
   the reference rasterizer proves too slow even for headless test runs, the
   fallback is to keep it for `gxtest` and the differential gate only and
   make the GL path the default everywhere else - at the cost of the
   byte-identical A/B gate, which would then compare state dumps rather than
   frames.
2. **The DSP microcode is game-supplied.** AX and Zelda cover nearly the
   whole commercial library, but a game shipping its own microcode would need
   the DSP interpreted. That is a phase of its own, and it is out of scope
   until a game needs it.
3. **RELs make static analysis harder than on any earlier console.** Code
   arrives from disc, relocated, at addresses decided at run time. The
   profiling step is not optional here, and the interpreter fallback will
   carry more traffic than it does on GBA or SNES.
4. **No disc image yet**, so P6 cannot close and the P4 analyzer has no real
   input to measure against. P1 to P3 do not depend on it.
5. **devkitPPC is a build-time dependency of the tests**, not of anything we
   ship. The vector generator and any test DOL we write need it. CI must not
   need it: checked-in vectors and generated DOLs cover CI, and the devkitPPC
   path is developer-only.
