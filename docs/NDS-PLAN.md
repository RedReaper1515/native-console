# Nintendo DS backend: plan

Started 2026-10-05 on branch `feature/nds-backend`. This follows
[ADDING-A-CONSOLE.md](ADDING-A-CONSOLE.md): the contract, the phases P0-P8 and
their gates are the ones described there. This file says what the DS adds,
what we reuse from other projects and from our own GBA and GameCube backends,
and how each piece gets tested.

The family id is `nds`: `ndsrecomp`, `runtime-nds/`, `tests/nds/`,
`tools/nds/`, extension `.nds`.

The first game is **The Legend of Zelda: Phantom Hourglass (USA)**, game code
`AZEE`, SHA-1 `4c8f52dd719918bbcd46e73a8bae8628139c1b85`. That is the exact
ROM the [zeldaret/ph](https://github.com/zeldaret/ph) decompilation targets
(`ph_usa.sha1`), so its symbols apply to our dump without adjustment.

## Ground rules

1. **License.** Native Console is MIT. We copy code only from MIT, BSD,
   Apache-2.0, zlib or CC0 sources and record every port in `THIRD_PARTY.md`.
   GPL emulators (melonDS, DeSmuME, NooDS, dust, DSVita) are run as separate
   oracle programs or read as behaviour references, never copied. Unlicensed
   test ROMs (armwrestler, rockwrestler) are fetched for local runs and never
   committed. PolyForm Noncommercial code (mstan/ndsrecomp) is read for ideas
   only.
2. **One semantics source.** One set of force-inlined instruction handlers
   serves the interpreter and the generated C, for both CPUs. The ARM7 is the
   ARM7TDMI core from `runtime-gba`. The ARM9 is the same handlers with the
   ARMv5TE additions and differences selected by a compile-time architecture
   parameter, not a second core.
3. **Guest state is explicit**, as on GBA: registers and flags in a context
   struct per CPU, every memory access through that CPU's bus.
4. **Interpreter from day one.** It runs anything the recompiler did not
   compile and is the differential oracle for what it did.
5. **No Nintendo firmware.** No BIOS dump, no firmware dump. The two BIOSes
   are the DraStic replacement (FreeBIOS, BSD-2-Clause), assembled from its
   source and run, and recompiled, like any other guest code. The boot is a
   direct boot and the firmware's user settings are synthesised. A user's
   own dumps may be accepted as options for cross-checking; they are never
   required and never shipped. See [The BIOS](#the-bios).
6. **Evidence layers stay labelled** and runs **fail closed**
   ([AGENTS.md](../AGENTS.md#accuracy-workflow)).

## What the DS changes

Four things are new compared with the consoles already supported. Each is
closer to something we have done than it first looks.

- **All code runs from RAM.** Nothing executes from the cartridge. The ARM9
  binary is loaded to `0x02000000`, the ARM7 binary to `0x02380000`, and
  overlays are loaded over each other on demand. The images are still fixed
  bytes in the ROM, so they recompile ahead of time; what changes is that a
  native function is valid only while the bytes it was compiled from are in
  RAM. This is the GameCube `.rel` problem (code loaded at run time) plus the
  GBA rule for RAM traces (check against live RAM before running natively).
  Phantom Hourglass: one 263 KiB compressed ARM9 binary, ITCM and DTCM
  autoload blocks, and 62 compressed overlays totalling 3.4 MiB that share
  `0x02077360-0x0219b120`; the decompilation groups them into 13 slots
  whose members replace each other.
- **Two CPUs on one machine.** ARM9 (ARM946E-S, ARMv5TE, 67 MHz) and ARM7
  (ARM7TDMI, 33.5 MHz) share main RAM and talk through IPCSYNC and the IPC
  FIFO. The SA-1 rule applies: the lagging CPU is caught up at the same
  points whether the other is native or interpreted, and always stops on an
  instruction boundary. Both CPUs are recompiled; the ARM7 binary is not
  replaced with C (see [Open decisions](#open-decisions)).
- **A 3D GPU.** Geometry engine (matrix stacks, lighting, clipping, the
  command FIFO with its timing and IRQ) and a scanline polygon rasteriser
  with its own rules for edge slopes, fill, depth, edge marking, fog and
  anti-aliasing. Software first, as on GameCube.
- **A different shape of machine for the player.** Two screens, a touch
  screen, a microphone and a lid. Phantom Hourglass needs all of them: the
  whole game is stylus-driven, it asks the player to blow and to shout, and
  one puzzle is solved by closing the DS.

Everything else is a superset of the GBA: the two 2D engines extend the GBA
PPU, timers and DMA are the GBA's, and the ARM7 is the GBA's CPU.

## What we reuse

Licenses checked with `gh api repos/OWNER/REPO` on 2026-10-05.

### Copyable (recorded in `THIRD_PARTY.md` when ported)

| Source | License | What we take |
|---|---|---|
| [zeldaret/ph](https://github.com/zeldaret/ph) | CC0-1.0 | For the P6 game: 24,608 function symbols (21,210 ARM, 3,398 Thumb; 4,118 named) with sizes, 11,557 data symbols, and 151,801 relocations (calls, with the ARM/Thumb mode of each target, and 68,360 data loads) for the main binary, ITCM, DTCM and all 62 overlays, each with a module hash. Used as analyzer input (`--symbols`), as the oracle for our own function discovery, and as names in generated code and for mods. [zeldaret/st](https://github.com/zeldaret/st) (CC0) is the same for Spirit Tracks. |
| [AetiasHax/ds-decomp](https://github.com/AetiasHax/ds-decomp) (`dsd`) | MIT | `dsd init` analyses any DS ROM with no input and writes the symbols/relocations/overlay config in the format above. Run as a tool to get the same data for games that have no decompilation; its analysis rules (function boundaries, literal pools, jump tables, the SDK's overlay table) are portable into our analyzer. |
| [AetiasHax/ds-rom](https://github.com/AetiasHax/ds-rom) | MIT | Reference for the ROM layout: header, FAT/FNT, overlay tables, BLZ (backwards LZ) decompression of the ARM9 binary and overlays, autoload blocks. |
| [AetiasHax/unarm](https://github.com/AetiasHax/unarm) | MIT | ARMv4T/v5TE/v6K disassembler: independent decoder oracle for the recompiler's listings (the role Capstone has for GBA). |
| [skylersaleh/SkyEmu](https://github.com/skylersaleh/SkyEmu) `src/nds.h` | MIT | The one permissively licensed DS core in C (7,456 lines): bus and I/O, card and AUXSPI, firmware/touch/RTC SPI, IPC, DIV/SQRT, geometry engine and a triangle rasteriser, direct boot without dumps. Reference for device behaviour, and a source of ports where our own code has nothing yet. Also `nds_rom_database.h`: game code to save type and size. Its DS support is labelled beta, so nothing is taken without a test. |
| DraStic BIOS (FreeBIOS) by Gilead Kutnick, in [melonDS](https://github.com/melonDS-emu/melonDS/tree/master/freebios) | BSD-2-Clause | Replacement ARM7 and ARM9 BIOS source. Assembled, it is the BIOS our games boot with (see [The BIOS](#the-bios)). |
| [Jaklyy/polyrastertest](https://github.com/Jaklyy/polyrastertest) | MIT | 3D rasteriser tests with reference data recorded on hardware. Prebuilt ROM. |
| [StrikerX3/nds-interp](https://github.com/StrikerX3/nds-interp) | MIT | Hardware captures of every edge slope and of attribute interpolation, with a reference `slope.h`. The rasteriser's edge walker is checked against the captures. |
| [StrikerX3/armajitto](https://github.com/StrikerX3/armajitto) | MIT | ARMv4T/v5TE recompiler library; second opinion on ARMv5TE semantics. |
| [mstan/arm-recomp-core](https://github.com/mstan/arm-recomp-core) | MIT | ARMv4T and ARMv5TE decode, IR and reference interpreter split out of ndsrecomp. Cross-check for ARMv5TE decode; we keep our own handler-based core. |
| [BlocksDS](https://github.com/blocksds/sdk) tests and libnds | Zlib / MIT / CC0 per file | 46 hardware test programs (IPC FIFO, IRQ handling, MPU and TCM, SWI calls, cache ops, DIV/SQRT, compressed textures). Source only: built in the `skylyrac/blocksds` Docker image. Also the SDK our own test ROMs are built with, so they can be committed. |
| [SingleStepTests/ARM7TDMI](https://github.com/SingleStepTests/ARM7TDMI) | MIT | The ARM7's per-instruction vectors, already fetched for GBA into `roms/gba/sst/`. |
| [x0cero/nds](https://github.com/x0cero/nds), [Atem2069/entr](https://github.com/Atem2069/entr) | MIT | Small from-scratch emulators (Rust, C). x0cero's notes on what Pokémon Platinum needs from the Wi-Fi registers are the useful part. |

### Tools and references only

| Source | License | Use |
|---|---|---|
| [melonDS](https://github.com/melonDS-emu/melonDS) | GPL-3.0 | The accuracy reference, above all for the 3D rasteriser. Separate oracle program for frames; source read as a behaviour reference. |
| [DeSmuME](https://github.com/TASEmulators/desmume) | GPL-2.0 | Scriptable oracle (Lua; `py-desmume` runs it headless) for memory and register probes at a checkpoint. |
| [NooDS](https://github.com/Hydr8gon/NooDS), [dust](https://github.com/kelpsyberry/dust), [DSVita](https://github.com/Grarak/DSVita) | GPL-3.0 | Behaviour references. NooDS and DSVita replace the ARM7 with native code; read for how that is done if we ever want it. |
| [RetroPortingToolKit/ndsrecomp](https://github.com/RetroPortingToolKit/ndsrecomp) | PolyForm Noncommercial (parts MIT, parts GPL-3.0) | The one existing DS static recompiler: SM64DS, Mario Kart DS and Metroid Prime Hunters run. Read for ideas only. It needs BIOS and firmware dumps by default and vendors melonDS's GPU; we need neither. Proof that the approach works on this console. |
| [Atem2069/armwrestler-fixed](https://github.com/Atem2069/armwrestler-fixed), [Arisotura/arm7wrestler](https://github.com/Arisotura/arm7wrestler) | none | ARM/Thumb/ARMv5TE instruction tests on the ARM9 and on the ARM7. Fetched, never committed. |
| [RockPolish/rockwrestler](https://github.com/RockPolish/rockwrestler) | none | ARMv5 additions and interworking, IPCSYNC, IPCFIFO and its IRQs, DIV/SQRT, WRAMCNT, VRAMCNT, TCM. Fetched, never committed. |
| [StrikerX3/nds-tests](https://github.com/StrikerX3/nds-tests) | none | 2D window tests. Source only. |
| pret Pokémon decompilations | none | Not used. |
| GBATEK (problemkaputt.de), [Jaklyy/ndsdoc](https://github.com/Jaklyy/ndsdoc) (CC0), the melonDS blog | reference | Hardware reference. GBATEK first. |

### From this repository

| What | Where | How it carries over |
|---|---|---|
| ARM7TDMI handlers, decode classifier, banking, exceptions (2.25 M vectors pass) | `runtime-gba/include/gba_cpu_*.h`, `src/gba_cpu*.c` | The ARM7 as is. The ARM9 adds an architecture parameter: BLX, CLZ, QADD/QSUB/QDADD/QDSUB, SMULxy/SMLAxy/SMULWy/SMLAWy/SMLALxy, LDRD/STRD, PLD, BKPT, CP15, interworking on loads into PC, the ARMv5 LDM/STM and multiply-flag rules, vectors at `0xFFFF0000`. |
| HLE BIOS (decompression, CpuSet, math, IntrWait, halt) | `runtime-gba/src/gba_bios.c` | Not carried over as the default: the DS boots an assembled replacement BIOS instead. The C routines are the starting point if a hot software interrupt is ever replaced (P8). |
| 2D renderer: text and affine backgrounds, sprites, windows, blending, mosaic | `runtime-gba/src/gba_ppu.c` | The base of each 2D engine. New: extended affine and large bitmap backgrounds, extended palettes, VRAM bank mapping, 3D as BG0, display capture, master brightness, bitmap sprites. |
| Timers, DMA, I/O table as data | `runtime-gba/src/gba_timer.c`, `gba_dma.c`, `gba_io.c` | Same devices, two instances, new DMA start modes. |
| Analyzer and emitter for ARM/Thumb, hashed entry lookup, `--differential`, `--coverage`, traces carrying RAM images checked before native use | `recompiler-gba/src/`, `runtime-gba/src/gba_recomp.c`, `gba_runner.c` | The recompiler's core. New: modules (main, ITCM, overlays, ARM7) instead of one ROM image, and dispatch that depends on which module is loaded. |
| Code loaded at run time, software rasteriser then GPU renderer, graphics thread, save states with exact-resume tests, `--shown-log` | `runtime-gc/` | The pattern for overlays and for the 3D pipeline. No code is shared: the GPUs have nothing in common. |
| SDL frontend, overlay menu, remapping, display filters, CRT, texture packs, RTC, link, save states | `runtime-gba/platform/`, `runtime/vendor/imgui` | The frontend. New: two screens and their layouts, touch, microphone, lid. |

## Per-instruction vectors for the ARM9

There is no public per-instruction vector set for ARMv5TE; SingleStepTests
covers the ARM7TDMI only. The P1 gate is built from three parts.

1. **The ARM7 core passes the ARM7TDMI vectors**: all 2,250,000, fetches
   and pipeline included. The handlers are one template
   (`nds_cpu_impl.inc`) compiled twice, so this also checks every line the
   two processors share.
2. **The ARM9 core passes a generated ARMv5TE set.**
   `tools/nds/gen_armv5_vectors.py` runs each test instruction once on
   unicorn's ARM946 model, which is QEMU's ARM front end, and writes the
   state before and after and the data accesses in order, in the
   SingleStepTests format: 53 classes of 10,000 tests. QEMU was chosen over
   the MIT interpreters the first draft of this plan named (armajitto,
   arm-recomp-core) because it is by far the most exercised implementation
   of the architecture, and it is one `pip install`, so anyone can
   regenerate the files. It is GPL, run as a tool; only its output is used.
3. **The wrestler ROMs** pass on both processors. They need the machine, so
   this part closes with P2.

What part 2 is evidence of, and what it is not. QEMU implements the
architecture, not the ARM946E-S. Where the manual says UNPREDICTABLE, what
QEMU does says nothing about the DS, so the generator stays inside what
ARMv5TE defines: aligned addresses, no R15 where the manual forbids it, no
base register in a written-back list, no reserved PSR bits. What the DS does
outside that (and it is deterministic there) is pinned by
`tests/nds/test_nds_cpu.c`, 232 checks that each name their source: GBATEK,
rockwrestler's LDM/STM cases (which pass on hardware), or the ARM ARM.
Neither part 2 nor those checks is hardware evidence for the ARM9; part 3
is.

Left out of the vectors because unicorn cannot show them: taking an
exception (it stops instead of entering the handler), CP15 (its generic
ARM946 has other cache and TCM ID values), and a Thumb BL as a pair.

Where QEMU and the DS part ways, found while building the set and handled
in the generator:

| QEMU | The DS's ARM9 | How the generator deals with it |
|---|---|---|
| Unaligned LDR reads four bytes at the odd address | Reads the aligned word and rotates it | Aligned addresses only; the rotation is a directed check |
| Exception return (S with Rd = R15) only for SUBS and MOVS; other opcodes set flags and branch | Every opcode copies SPSR to CPSR | Only SUBS and MOVS are generated; ADDS and EORS are directed checks |
| A BL suffix with an odd LR ends in ARM state | Stays in Thumb state | LR is always even, as a prefix leaves it |
| A BLX suffix with an odd offset executes | Undefined | Even offsets only; the odd one is a directed check |
| CBZ, IT and other Thumb-2 halfwords execute | Undefined | The "undefined" classes keep a test only if QEMU rejects the encoding too |
| LDM with the base in a written-back list: no writeback | Writeback unless the base is the last of several | Not generated; rockwrestler's cases are directed checks |
| MSR can write CPSR bit 8 | Reserved | PSR values use only the bits ARMv5TE has |

Two faults in unicorn 2.1.4 itself turned up the same way, each able to
corrupt vectors without any error: it keeps running stale translations
after code is rewritten, and after translating (not running) a Thumb block
that ends inside an IT instruction it stops honouring the one-instruction
limit. The generator's memory layout removes the conditions for both (see
the comment at the top of the script), and a canary aborts generation if
either ever shows.

A mutation check backs the claim that these tests would notice a wrong
core: 12 deliberate faults in the ARM9 paths (saturation direction, a shift
count, LDRD word order, the BLX link value, and so on) were each caught, 11
by the vectors and all 12 by the vectors and directed checks together.

## The BIOS

The DS has two small BIOS ROMs: 4 KiB for the ARM9 at `0xFFFF0000` and
16 KiB for the ARM7 at `0`. Games use them for interrupt dispatch and for
about thirty software interrupts: division, square root, memory copies,
decompression (some of it through callbacks into the game), CRC, waiting
for interrupts, and the sound driver's sine, pitch and volume tables.

The playbook says to emulate a BIOS in C. For the DS we run a replacement
instead: the DraStic BIOS by Gilead Kutnick (FreeBIOS), BSD-2-Clause,
assembled from the source melonDS distributes
(`tools/nds/build_freebios.py`; the result is byte for byte the image
melonDS embeds). It is what melonDS and SkyEmu boot when the user has no
dumps. The reasons:

- Interrupt dispatch, the part every game depends on every frame, is then
  real ARM code executing on our CPU cores rather than a C imitation of it.
- It is static code at fixed addresses, so `ndsrecomp` compiles it like a
  game module and it costs nothing at run time.
- Nothing of Nintendo's is needed or shipped, which is what the rule is
  for. The port is recorded in `THIRD_PARTY.md`.

It is not a complete BIOS, and where it differs from Nintendo's it is
wrong, not a matter of taste. Found by running the built images
(`roms/nds/built/freebios/SWI_CHECK.txt`):

| Software interrupt | FreeBIOS | Nintendo's |
|---|---|---|
| 09h Div | Remainder is unsigned (-1234 / 10 leaves +4); division by zero never returns | Signed remainder |
| 0Dh Sqrt | 0 for arguments from 0x80000000 up (signed compare) | Correct |
| 12h LZ77 by callback, 15h run-length by callback | Read memory at r0; the callbacks are never called | Call back into the game |
| 13h Huffman by callback | Returns at once | Decompresses |
| 08h SoundBias | Ignores its arguments | Ramps to the level asked for |
| 1Fh CustomHalt / CustomPost | Jumps to a wild address (the table is one slot short) | Writes HALTCNT / POSTFLG |
| 00h SoftReset | Unusable | Restarts the program |
| Undefined instruction, aborts | Loop forever | Hand over to the program's debug handler |

melonDS runs commercial games on the same image, so most games do not
meet these. One that does needs that interrupt in C on top of the image
(P8), checked against a user's own dump: `--bios9 FILE` and `--bios7 FILE`
accept one. arm7wrestler's "ARM V5TE" page and BlocksDS's
`system_stack_smash_protection` need the debug handler and are not gated
on.

## Layout

```text
runtime-nds/
  include/nds_cpu*.h        shared handlers, ARMv4T/ARMv5TE parameter, CP15
  include/nds.h             machine state: two CPUs, buses, devices
  src/nds_cpu*.c            interpreter for both CPUs
  src/nds.c                 scheduler, video timing, run loop, dispatcher
  src/nds_bus9.c, nds_bus7.c  memory maps, TCM, WRAMCNT, VRAM banks
  src/nds_io.c              I/O register table (per CPU)
  src/nds_ipc.c, nds_math.c, nds_timer.c, nds_dma.c
  src/nds_card.c            card commands, AUXSPI saves (EEPROM, FLASH, FRAM)
  src/nds_spi.c             firmware flash, touch controller, power management, RTC
  src/nds_bios_data.c       the assembled replacement BIOSes (FreeBIOS, BSD-2-Clause)
  src/nds_boot.c            direct boot, BLZ, synthesised firmware settings
  src/nds_ppu.c             the two 2D engines
  src/nds_gx.c, nds_gx_raster.c  geometry engine, software rasteriser
  src/nds_spu.c             16 channels, capture
  src/nds_wifi.c            register stub: present, no network
  src/nds_recomp.c          module install (hash check), loaded-module tracking, entry lookup
  src/nds_runner.c          headless runner, --differential
  platform/nds_sdl.cpp      SDL2 frontend, screen layouts, touch, overlay menu
  tools/nds_run.c, nds_play.c
recompiler-nds/src/         rom (header, FAT, overlays, BLZ), analyzer, emitter, CLI (ndsrecomp)
tests/nds/                  vector harness, device unit tests, ROM baseline
tools/fetch_nds_tests.py    pinned downloads into roms/nds/ (gitignored)
tools/nds/                  run_nds_tests.py, recomp_suite.py, compare_frame.py
```

## Phases

### P0: research, licenses, test data

- This plan, `tools/fetch_nds_tests.py`, `.nds` in `.gitignore`.
- **Gate:** the fetch script runs clean twice and the second run does
  nothing.

### P1: CPU cores

- Move the ARM7TDMI handlers into `runtime-nds` behind the architecture
  parameter and add ARMv5TE and CP15.
- `tests/nds/sst_arm.c`: the GBA harness, reading ARM7TDMI vectors for the
  ARMv4T configuration and the generated set for ARMv5TE.
- **Gate:** [the three parts above](#per-instruction-vectors-for-the-arm9),
  in Release, Debug, AddressSanitizer and UBSan builds. The wrestler ROMs
  need a little of P2 (LCDC framebuffer mode, keys), so their part of the
  gate closes with P2.

### P2: machine model and headless runner

- Both memory maps: main RAM and its mirrors, ITCM and DTCM (CP15-relocated,
  load mode), shared WRAM (WRAMCNT), ARM7 WRAM, VRAM banks A-I (VRAMCNT, and
  banks mapped to the ARM7), palette, OAM, both BIOS areas, I/O.
- Two-CPU scheduler on one event queue. IPCSYNC, IPC FIFO and their IRQs.
  IE/IF/IME per CPU, HALT, POSTFLG. Timers and DMA per CPU with the DS start
  modes (card, HBlank, VBlank, display start, geometry FIFO).
  DIV and SQRT units. KEYINPUT, EXTKEYIN (X, Y, pen, lid).
- Card: header, the `B7`/`B8` commands and their timing, ROMCTRL, card DMA.
  AUXSPI saves, type from the protocol and from the save database.
- SPI: firmware flash (synthesised settings), touch controller, power
  management. RTC.
- The replacement BIOSes mapped at their addresses. Direct boot: header copy to `0x027FFE00`,
  user settings, the register state the firmware leaves.
- `nds_run`, `tools/nds/run_nds_tests.py`, the baseline CTest.
- **Gate:** armwrestler, arm7wrestler and rockwrestler pass in the
  interpreter (every rockwrestler group: ARMv5, IPC, DS math, memory). The
  BlocksDS tests that report a verdict pass. Every failing ROM is listed
  with the phase that owns it.

### P3: video

- 2D engines A and B: all background modes, extended palettes, bitmap
  sprites, windows, blending, master brightness, display capture, the
  display-swap bit. Per-scanline latching.
- Geometry engine: command FIFO and PIPE, packed commands, matrix stacks and
  their overflow flags, lighting, texture coordinate transforms, clipping,
  viewport, box/position/vector tests, swap buffers and its timing, the FIFO
  IRQ.
- Software rasteriser: edge slopes, fill rules, the depth buffer in both
  modes, texture formats including 4x4 compressed, modulation, decal, toon
  and highlight, shadow polygons, alpha test and blending, edge marking,
  fog, anti-aliasing, the rear plane, the 2048-polygon and 6144-vertex
  limits.
- **Gate:** polyrastertest passes; the edge walker reproduces the nds-interp
  hardware captures; the window tests pass; frames of scripted Phantom
  Hourglass scenes match melonDS's software renderer
  (`tools/nds/compare_frame.py`), with every remaining difference listed.
  Each register field that selects among N things has a test for all N.

### P4: recompiler

- `ndsrecomp`: parses the ROM, decompresses the ARM9 binary and overlays,
  and treats the main binary, ITCM, each overlay and the ARM7 binary as
  modules, each with its load address and a hash of its bytes.
- **Analyzer.** Entry points from the header, the exception vectors, the
  SDK's autoload and overlay tables, symbols (`--symbols DIR` in the
  ds-decomp format), traces and profiles. Relocations, where given, say
  which words are data and which mode each call target is in.
- **Dispatch.** Keyed by CPU, address and mode. A function runs only while
  the bytes it was compiled from are in memory (see "How overlays are
  handled" under Status, which replaced the per-module tracking first
  planned here).
- Both CPUs are compiled. Cycle charging matches the interpreter; either CPU
  yields when the other is due.
- `--differential`, `tools/nds/recomp_suite.py`, CTest `nds_recomp_*`.
- **Gate:** every test ROM gives the same verdict recompiled as interpreted
  and is clean in `--differential`. Our function discovery, run without
  symbols on Phantom Hourglass, is compared against the decompilation's
  24,608 functions, and the misses are listed.

### P5: audio, saves, frontend

- Sound: 16 channels (PCM8, PCM16, IMA-ADPCM, PSG, noise), volume and pan,
  loop and one-shot, the two capture units, SOUNDBIAS. A mixer output test.
- Save types by protocol, with the save test and a rebuild that keeps it.
- SDL frontend: screen layouts (stacked, side by side, one large and one
  small, single screen with a swap key) including ultrawide; touch from the
  mouse, a real touch screen and a controller-driven cursor; microphone from
  the host device or a bound "blow" button; a lid binding; display filters.
- The overlay menu, including the DS-only items: layout, touch cursor, lid,
  microphone.
- Save states with a version, a layout fingerprint, scrubbed host pointers
  and an exact-resume test.
- **Gate:** as in the playbook, plus `--menu-selftest` covering the layout,
  touch and lid bindings.

### P6: hub integration and the first game

- `hub/src/console_nds.cpp`, settings, the profiling step.
- **Gate:** Phantom Hourglass imports, builds, plays and gets a cover; its
  save survives a rebuild; `--differential` is clean over 6,000 scripted
  frames (touch input scripted); the native share is reported; the window
  log matches the headless frames. The stylus, the microphone and the lid
  are each exercised at a point where the game needs them.

### P7, P8

As in the playbook. P7 waits until the Windows build works.

## Checklist coverage

| Nuance | Test |
|---|---|
| ARMv4T semantics on both CPUs | ARM7TDMI vectors; armwrestler, arm7wrestler |
| ARMv5TE additions, interworking on loads to PC, LDM/STM edge cases | Generated vectors; rockwrestler ARMv5 group |
| CP15, ITCM/DTCM relocation and load mode | rockwrestler TCM; BlocksDS `tcm_location`, `dtcm_fixed_size`, `mpu_regions` |
| WRAMCNT, VRAMCNT, VRAM mapped to the ARM7 | rockwrestler memory group |
| IPCSYNC, IPC FIFO, their IRQs, FIFO full and empty errors | rockwrestler IPC group; BlocksDS `fifo_full`, `transfer_region` |
| DIV and SQRT units | rockwrestler DS math; BlocksDS `hw_sqrtf` |
| Interrupt dispatch and software interrupts through the replacement BIOS | BlocksDS `irq_handling`, `swi_calls`, `vectorbase`; rockwrestler |
| BLZ decompression | Module hashes from the decompilation config (65 modules) |
| Two-CPU determinism | Same state hash with either CPU native or interpreted, all four combinations |
| Overlay load, unload and overlap | Unit test with synthetic modules; `--differential` across Phantom Hourglass scene changes |
| 2D windows | StrikerX3 window tests |
| Rasteriser edges and interpolation | nds-interp captures |
| Rasteriser fill, depth, blending, edge marking | polyrastertest |
| Compressed textures | BlocksDS `compressed_texture_combined`; frame comparison |
| Card read timing, save protocol | Unit tests on the command state machines; the save round trip |
| Sound mixer output stage | Unit test (DC offset, clipping, capture) |
| Wi-Fi registers present | Phantom Hourglass and a second game reach gameplay; register trace compared with DeSmuME |

Not covered by any test we have found: geometry FIFO timing, 2D engine
edge cases beyond windows, sound capture timing. Our own BlocksDS-built test
ROMs fill these as games show them to matter, and are committed.

## Decisions

Made with the user on 2026-10-05.

1. **ROM data.** The code modules are always embedded. The whole ROM is
   embedded up to 128 MiB (GBA already embeds 32 MiB); above that the file
   system is packed into `ID.ndsdata` next to the executable, listed in
   `install_files()`, as GameCube does.
   *Changed 2026-10-06:* the side file was not built. A 512 MiB ROM, the
   largest a DS card holds, embeds like any other: recompiled in 0.9 s,
   built in 8 s, a 537 MB executable that runs. One file is simpler for
   the player, so every ROM is embedded whole.
2. **The ARM7** is recompiled from the game's own ARM7 binary. It is exact,
   it is the ARM7TDMI we already pass every vector on, and it keeps
   `--differential` meaningful for the whole machine. Replacing it with C
   (as NooDS and DSVita can) is a later speed option.
3. **Oracles**, installed under `../tools/` and run as separate programs:
   melonDS 1.1 (`melonDS-1.1-win64`), DeSmuME 0.9.13
   (`desmume-0.9.13-win64`), and a Python environment (`nds-venv`) with
   py-desmume 0.0.9 for scripted runs and unicorn 2.1.4 for the vector
   generator.
4. **Source-only test ROMs** are built in the `skylyrac/blocksds` Docker
   image, which is pulled.

## Status

Merged to `main` on 2026-10-06 (pull request 101). The same day, `main`
was built with GCC 10.3 in the Steam Runtime SDK container and the 18 DS
tests that need no fetched data passed there; that is a check of the shared
build, not P7.

| Phase | State |
|---|---|
| P0 | 2026-10-05: licenses checked for about 60 repositories; `tools/fetch_nds_tests.py` fetches the suites and its second run is a no-op. Phantom Hourglass (USA) hash matches the decompilation's. |
| P1 | 2026-10-05: gate met. ARM7: 2,250,000 / 2,250,000 SingleStepTests ARM7TDMI vectors. ARM9: 530,000 / 530,000 generated ARMv5TE vectors (53 classes) and 232 directed checks, in MSVC Release, Debug and AddressSanitizer builds and gcc 10, gcc 14 and clang 11 with sanitizers and at `-O2`. Part 3, 2026-10-06: armwrestler shows OK on all 77 rows and rockwrestler on all 23 tests (CTest `nds_wrestlers`, 101 of 101 verdict rectangles; DeSmuME itself fails six of rockwrestler's); arm7wrestler shows OK on every ARM and Thumb row on the ARM7. Its "ARM V5TE" page is not counted: those are ARMv5 instructions on an ARMv4 CPU, and the page needs the BIOS's debug handler. The gcc run found a real fault: Thumb register shifts passed the shifter and the carry it writes as two arguments of one call, an order C leaves open. The GBA core had the same lines (fixed on main in PR 98). |
| P2 | 2026-10-06: gate met. Both memory maps, I/O, timers, DMA, IPC, maths unit, card and card DMA, two-CPU scheduler, direct boot with the replacement BIOS, SPI (firmware flash built from synthesised settings, touch, power), RTC, cartridge saves by chip (table of 6,776 games from SkyEmu's MIT database, detection for the rest), `nds_run`. Unit tests: machine 684 checks, SPI 5,238, save 555, firmware 206, RTC 120, sound 5,633, 2D 14,242. **Test ROMs** (`tools/nds/run_nds_tests.py`, CTest `nds_test_roms_baseline`, 12 s): 71 judged runs over 34 ROMs (the BlocksDS hardware tests, arm7wrestler, two window tests), each read the way it reports: text on a console layer, the debug log, a picture hash, verdict rectangles. 70 pass. The one failure, `system_sbrk/arm7-log`, is the runner's: libnds's ARM7 prints with the no$gba opcode (`mov r12, r12` and inline text), and only the I/O-port form is printed; the same test's on-screen verdict passes. Three more entries are held back by `requires`: `stack_smash_protection` and arm7wrestler's V5TE page need the BIOS to dispatch debug exceptions, which the replacement does not, and `cache_data_cache_ops` needs a data cache, which is not modelled. 15 built ROMs cannot report a verdict here (`--list` says why for each: they wait for input with no stated expectation, need a DSi, or draw without judging). The tests found three faults, now fixed and pinned by unit tests: Thumb LDMIA with the base in its list wrote the base back on the ARM9; VRAMCNT and WRAMCNT did not read back; an ARM9 halted with CPSR.I set woke up. Open: main-memory display DMA, palette and OAM gating for an unpowered engine. |
| P3 | 2026-10-06: both 2D engines and the 3D engine (geometry and software rasteriser) are in. CTest: `nds_2d`, `nds_gx` (704 checks), `nds_gx_raster_full` (nds-interp's 198,404 measured scanlines, all exact), `nds_polyrastertest_rom` (the ROM through the whole machine ends "Tests Passed: 77/77"). BlocksDS's compressed-texture 3D test equals its CI reference picture pixel for pixel, and its text tests match theirs at the CI's own frame counts where the run is not CPU-bound. Phantom Hourglass, no input, against DeSmuME: the logo and notice screens are pixel-identical (250 of the first 250 frames, most at the same frame number); the 3D intro on both screens matches at a constant offset of 36 frames with a mean difference of 0.3 of 31 levels. Scripted, the save-file, name-entry and mode screens match apart from the nickname, and the prologue's pictures match pixel for pixel where the two runs are on the same page. The window was checked too: `--display-shot` through each layout, and a run driven by mouse clicks posted to the window (below, P6). The 21 black rows above and below the intro and the black first and last columns are the game's own; DeSmuME's frames have them too. Open: comparison against melonDS rather than DeSmuME (melonDS 1.1 has no headless frame output; it would have to be built as a tool), the list of remaining differences. |
| P4 | 2026-10-06: gate met. `tools/nds/recomp_suite.py` (CTest `nds_recomp_blocksds_system`, `nds_recomp_wrestlers`) profiles, recompiles and builds each test ROM as the hub would and requires the interpreter's verdict, its final state and a clean `--differential`: 103 of 103 runs (the BlocksDS tests, arm7wrestler, armwrestler, rockwrestler), and the 15 ROMs with no verdict agree in state and differential over 300 frames. Native share with a profile: the ARM9 99-100% on all but armwrestler (84.9%: its pages are reached by keys the built-in script does not press), the ARM7 83-100%; without one the ARM7 of a BlocksDS program is 0-49% native, because its start-up code moves it. The discovery comparison is below under "How code is found". Phantom Hourglass **without symbols**, built the way the hub builds it (profile, then recompile): 31,987 functions, 1,244,061 instructions, profiled in 30 s, generated in 4 s, built in 31 s. Its 1,800-frame intro and a 14,400-frame scripted route to the first scene on the ship both run 100.00% native on both CPUs (17,210 of 1.65 billion ARM9 instructions and 22,056 of 918 million ARM7 instructions interpreted on the route); the route ends in the same state, bit for bit, as the build made with the decompilation's symbols; `--differential` is clean over the route's first 6,000 frames (20.6 M slices, 68 s). With the decompilation's symbols and one round of trace feedback instead: 99.99% (ARM9) and 99.94% (ARM7) on the route. Speed, headless, 1,800 frames: 9.1 s recompiled against 11.0 s interpreted. CTest: `nds_analysis` (39 checks on the analyzer's rules), `nds_recompile_fixture` (the profile, the project and a relocated program, on the repository's own ROMs). |
| P5 | 2026-10-06: gate met, save states apart (not started; the menu does not offer them). Sound: 16 channels, capture, a 32,728.5 Hz ring; Phantom Hourglass plays its intro music with no clipping. Save chips by table or detection; a save made in one scripted run is file 1 on the next run's file-selection screen (`--dump-save`, `--load-save`), and the windowed game reads the file beside its executable. Frontend (`runtime-nds/platform`): four layouts with swap, Pixel Perfect and Fit, three filters, the stylus from the mouse, a finger or a controller's stick, held keys for the microphone and the lid, the overlay menu, `nds_play`. CTest `nds_input_layouts_and_bindings` (8,784 checks) and `nds_menu_selftest` (89 checks and 5 after a restart: the menu from the keyboard and from a controller alone, the game paused, bindings changed, saved and put back, stylus, microphone and lid reaching the game, settings and save across a restart). Windowed Phantom Hourglass: 3,000 frames at full speed, the audio queue never dry (mean 55 ms ahead, minimum 37 ms). Open: save states; the microphone needs a sample-rate source in `nds.h` (the frontend feeds it a level every 1,024 cycles by running the frame in slices itself); the clock follows emulated time, so it stops in the menu and runs ahead under fast forward; changing the nickname or language rebuilds the firmware image, dropping what a game stored there; untested by hand: a real controller, a real touch screen, this computer's microphone. |
| P6 | 2026-10-06: gate met on Windows; `supported()` is on. Phantom Hourglass, the user's own dump, through the hub with a scratch library: imports (probe: header, secure area, DSi-only titles refused), builds in 1 min 10 s without symbols, gets its title screen as cover (frame 3,000). In its window, mouse clicks posted to it skip the intro, pass the title and reach File Selection, which lists the file from the save beside the executable ("Player", three hearts): input, picture and save all go through the window's own paths. The save is byte-identical after the run and after a rebuild from the hub. On the installed executable: `--differential` clean over 6,000 scripted frames (touch input; 78 s); 100.00% native on both CPUs over the 14,400-frame route (17,210 and 22,056 instructions interpreted). Closing the lid for 150 frames at the title freezes the picture for 153 frames and the game resumes, recompiled and interpreted ending in the same state. **Compile for export** makes a working Windows zip (73 MB: the executable, SDL2.dll, a README), run from an unpacked copy. Not met as worded: the microphone is exercised by the device tests and the menu self-test, not at a point in the game that needs it (the first is hours in); likewise the lid beyond sleep and wake. |
| P7 | Not started: Linux, SteamOS and macOS wait until the Windows build has been played (`tools/consoles.sh`, `build_steamos.ps1`, the workflows). |
| P8 | Not started. Known: the ARM9's access costs are an average and BlocksDS's CPU-bound tests finish later here than on their CI (up to three times later through the BIOS, whose fetches cost four cycles each); each instruction is a call into a shared handler, which is most of the CPU time and of the 129 MB executable; `--inline-handlers` and call chaining are the levers. |

### How code is found

Measured on Phantom Hourglass against the 24,608 functions its
decompilation lists, with `--symbols` not given (2026-10-06):

| Step | Functions found as entries | Native share of the intro (ARM9 / ARM7) |
|---|---|---|
| Calls and branches from the entry points, the overlays' initialiser tables, pointers in data to a `push {.., lr}` | 22,013 (89.5%) | 86.82% / 81.67% |
| The same pointer scan over the DTCM block too (the SDK's 3D library keeps its tables of rendering functions there) | 22,069 (89.7%) | 98.88% / 81.67% |
| Leaf functions pointed at from data (below) | 24,358 (99.0%) | 99.75% / 81.73% |
| The hub's profile trace | | 100.00% / 100.00% |

- **Leaf functions.** A getter, an empty virtual function or a veneer
  pushes nothing, so the prologue test cannot tell a pointer to one from a
  pointer to data. `Analyzer::looks_like_leaf` takes a pointed-at address
  when what is before it ends something (a return, an unconditional jump, a
  literal pool, alignment padding) and what is at it runs into a return or
  a jump within 24 instructions with no call, no stack frame and nothing
  undefined, the first instruction unconditional. Accepting one explores
  it, which can give its neighbour a boundary, so the candidates are gone
  through in address order until a round finds nothing. Of the entries
  this added, 2,331 are function starts in the decompilation, 1,333 are
  labels inside its functions, and 104 are not code as decoded.
- **What is still missed** (250 functions, 1.0% of the bytes): 212 have no
  pointer anywhere in the images (131 of them have an ordinary prologue, so
  nothing refers to them at all), and 38 are pointed at but fit neither
  rule (a leaf longer than 24 instructions, or one that does not start
  where something ends).
- **The profile.** `ndsrecomp ROM --profile FILE` plays 6,000 frames with
  the interpreter under a built-in input script (stylus taps and strokes
  over a low-discrepancy sequence of points, the D-pad and every button)
  and writes every instruction that ran, with its opcode. What it finds
  that the rules above cannot is code reached through a pointer that was
  never a word in the image: the ARM7's interrupt handler (its address is a
  literal the start-up code stores into the BIOS's vector, and it begins
  with `mov ip, #0x04000000`), the return path the handler loads into LR,
  and the BIOS's own continuation after calling it. Those ran 18% of the
  ARM7's instructions. The frames are split over three starts with
  different taps and a reset between them, because a blind script that
  taps into a dead end stays there: on Phantom Hourglass two of three runs
  end in the Wi-Fi set-up utility. The script does not need to get far:
  no profile here got past the game's menus, and each still gave 100.00%
  on the route into the game, because what the profile adds is code that
  runs all the time.
- **The opcode in a trace line** ("CPU ADDRESS a|t COUNT OPCODE") lets an
  overlay take only the instructions it actually holds; overlays share
  addresses, and without it every overlay loaded there would be given the
  others' entry points.
- **Copies.** Code a program moves before running it (a homebrew start-up
  routine copying the ARM7 program into work RAM) is recognised from the
  trace: instructions that ran where no image has them, whose opcodes sit
  in an image a fixed distance away. The distance at least 16 of them agree
  on, with at least 80% of everything traced inside the moved image
  agreeing, gives a second image at that address, cut down to the pages
  that ran. A BlocksDS test ROM's ARM7 went from none to 96% native that
  way, with the interpreter's exact state. Only ARM-mode instructions vote.
- **Symbols** (`--symbols DIR`, a ds-decomp configuration) remain an
  option: they give modes, data ranges and names. The hub passes one placed
  in `<game>/profile/symbols/`.

### How overlays are handled

Not the way the P4 text above planned it. Nothing tracks which overlay is
loaded. Every native function carries a hash of the exact bytes it was
compiled from and runs only while guest memory holds those bytes at those
addresses; several functions may share an entry address, one per overlay
with code there, and the one in memory is the one that runs. A function
depends on nothing outside those bytes (calls and indirect jumps leave
through the run loop; literal pools are read when the instruction runs), so
a wrong guess by the analyzer can cost coverage but not correctness.
Checking is lazy: writes to words that hold compiled code are noted per
4 KiB page, and a function is compared again only when one of its pages,
or the memory map, has changed. In 14,400 frames of Phantom Hourglass that
is 6,520 comparisons for 286 M calls into native code.

### Numbers worth keeping

- Phantom Hourglass's timeline runs 36 frames behind DeSmuME's through
  boot (DeSmuME's card reads take no time) and is ahead of it by a few
  hundred frames at the end of the 10,000-frame prologue. Which of the two
  is nearer the console is not known; the ARM9's access costs are an
  average (`NDS9_CYCLES_*`), a P8 item.
- Phantom Hourglass through the hub (2026-10-06, scratch library): import
  and build 1 min 10 s (profile 29.8 s, recompile 3.8 s, configure 2.7 s,
  compile 30.8 s); the cover is frame 3,000, the title screen, captured in
  17.7 s; the executable is 129 MB, 64 MiB of it the ROM.
- Where a headless frame goes (`--profile`, recompiled, the intro): the
  CPUs 46%, the 3D rasteriser 24%, the 2D engines 11%, 3D geometry 9%.
  The CPUs' share is mostly the shared handlers decoding each opcode at run
  time and the run loop between calls; `--inline-handlers` and call
  chaining are the levers.

## Internal resolution (2026-10-08)

The 3D engine's picture drawn at two to four times the screens' size, for a
display with the pixels. `nds_set_internal_scale()` in `nds.h`; Internal
Resolution in the menu, `display.internal_scale` in the settings file,
`--internal-scale N` on a game's command line and on the headless runner,
whose `--dump-large FILE.ppm` writes what a display would show.

**The machine does not change.** It goes on drawing the console's own
picture exactly as before, and everything a game can see comes from that:
display capture, a captured picture used as a texture, timing. The larger
picture is drawn beside it and nothing reads it but the display. So the
state hash, the frames and every existing test are the same at any scale,
and the setting cannot break a game; what it costs is drawing the frame
twice.

**The 3D picture** (`nds_gx_render_scaled` in `nds_gx_raster.c`) is the same
rasteriser on a finer grid, with the same rules for which pixels an edge
owns, the same texture, blending, fog, edge-marking and anti-aliasing
steps. Three things are not the console's arithmetic, because that
arithmetic was built for 256 pixels:

- A vertex stands where the viewport division put it. The console drops
  the fraction of a pixel; `to_screen()` now keeps it (`NdsGxVertex.fx`,
  `fy`, in 256ths), and the larger picture uses it. This is most of why
  the picture is steadier as well as sharper.
- Edges are walked with one exact division (`slope_span_fine`). The
  console multiplies by a reciprocal cut to 18 bits, whose error grows
  with both of an edge's lengths: a fifth of a pixel across the screen,
  several pixels across a picture four times the size.
- The perspective weight has 16 fraction bits either way, where the
  console has 9 along an edge and 8 along a scanline: 8 bits would give a
  scanline 256 steps of texture however many pixels it has. Depth under
  Z-buffering is linear without the console's cut-down range.

The rear plane's bitmap, which has one pixel a screen pixel, is repeated.
The shadow stencil starts clear on every line of a larger picture; on the
console a line can inherit the stencil of the line two above, and keeping
that would make each line depend on the ones before it.

**The 2D engines** draw each line once, as before, and
`nds_2d_render_line_layers()` also hands back the line's layers as they
stood before stacking. `nds_2d_stack_scaled()` stacks them again around the
finer 3D pixels: priorities, windows, the 3D layer's own alpha, colour
effects and master brightness are worked out for each finer pixel, with
backgrounds and sprites repeated. A line where BG0 is not the 3D picture
is the screen's own line, repeated.

**3D on both screens.** A game that wants 3D on both screens gives engine
A to each screen by turns, has the capture unit take its picture, and
shows the other screen from last frame's capture through engine B. Phantom
Hourglass's opening does this, and shows the bottom screen's capture as a
direct-colour bitmap background (block C) and the top screen's as twelve
bitmap sprites (block D).

So a captured line that was drawn larger keeps a finer twin
(`FineCapture` in `nds_video.c`): the larger line as the capture unit
would have taken it, before master brightness and cut to five bits a
channel, with the address in video memory the capture unit wrote the line
at and the pixels it wrote. The 2D engine says, for a line it has just
drawn, where each pixel of a direct-colour bitmap shown as it is stored
was read from: a whole line for a background (`Nds2dLayers.direct_row`), a
pixel at a time for unscaled, unflipped bitmap sprites (`obj_src`). A
pixel read from bytes the capture unit wrote, which still hold what it
wrote, is stacked a second time around its twin, as a 3D pixel is around
the larger 3D picture. Anything else is the screen's own pixel: a capture
the game has drawn over, one it scales or flips, one used as a texture, a
capture that blends two sources.

**Which screen.** A screen is offered larger (`nds_fine_frame`) once every
line of it has been written in each of the last two frames, with a line
drawn larger among them. A screen with nothing drawn larger the frame
before is not written at all, so a screen of backgrounds and sprites alone
costs nothing; one that starts to show 3D is offered three frames later.

**Threads.** The larger picture's lines are drawn in bands of 24 by the
calling thread and helpers (`nds_gx_workers.c`, the Nintendo 64
rasteriser's pool; `NDS_GX_THREADS`, default half the processors up to
eight). A band also draws the line above and below it, for the depths and
polygon IDs edge marking compares, so bands do not depend on one another
and the picture is the same for any number of threads. The console's own
picture is drawn by the calling thread while the helpers have already
begun (`nds_gx_render_both`).

**Measured** (Phantom Hourglass, the 14,400-frame scripted route, headless,
20-core desktop, eight threads). The state at the end is
`7F840B64600CC9D5` at every scale, interpreted and recompiled, which is
the route's value from before this work. Over the route's 3D-heavy part:

| | 1x | 2x | 3x | 4x |
|---|---|---|---|---|
| interpreted, frames 3,000 to 6,000, ms a frame | 7.5 | 8.3 | 10.1 | 13.9 |
| recompiled, frames 8,400 to 14,400, ms a frame | 6.5 | 7.4 | 9.1 | 13.1 |

With one thread 3x is 22 ms a frame over the first 6,000 frames against
9.2 with eight (measured before the console's picture was drawn alongside).
The console's own rasteriser takes 3.8 ms of a frame there, and drawing
nine times the pixels takes about seven times as long: the work is in the
pixels, not in setting up polygons. 4x with sixteen threads: 11.8 ms
interpreted.

**Tests.**

- `nds_gx_raster`: the console's path still matches nds-interp's 198,404
  lines and polyrastertest's 77 scenes, and its hash over all 19,284 test
  frames is `D70493C314315CC9`, what `main` gave before. New, for the
  larger picture (86 checks): a picture of whole pixels - rectangles, a
  translucent one, a texture a texel a pixel, the rear plane's bitmap -
  comes out with every pixel repeated at 1x to 4x; a rectangle at half a
  pixel starts half a pixel further on, in larger pixels; a slanted
  triangle covers its area times the scale squared; an 886-polygon frame
  with fog and edge marking is the same from 1, 2, 5 and 16 threads; the
  engine's state is untouched.
- `nds_2d`: every line any of its tests renders with the 3D layer showing
  (2,144 of them: each blend, window, priority and brightness the tests
  set up) is stacked again at 2x and 3x around made-up finer pixels, and
  each finer pixel must be what the engine itself draws there when the
  screen's 3D pixel is that one. No pixel differs.
- `nds_machine`: two machines sent the same triangle, one with a 3x
  picture beside it. Framebuffer, video memory, main memory, the 3D engine
  and the clock are identical; the larger frame has nine times the
  triangle, edges that are not the screen's repeated, and follows engine A
  to the other screen. And the same two machines with 3D on both screens
  the way Phantom Hourglass does it (engine A by turns, a capture into
  block C shown as a bitmap background and one into block D shown as
  bitmap sprites): both screens have a larger frame with nine times the
  triangle in both kinds of frame, the console's picture and video memory
  stay identical, and where the capture is drawn over, the larger frame
  goes back to the screen's own pixels there and nowhere else.
- `nds_2d`, for captures: every line its tests render with a direct-colour
  bitmap shown as stored (18 with a background, 372 with sprites) is
  stacked again at 2x and 3x around made-up finer pixels for the bitmap,
  and each must be what the engine draws when those pixels are written
  into the bitmap itself. No pixel differs.
- By hand: Phantom Hourglass's route with `--dump-large` at 3x, looked at
  beside the console's frames, and `nds_play`
  on a BlocksDS 3D program with `--internal-scale 3 --display-shot`, whose
  shot at 768 by 1152 is the headless larger picture pixel for pixel.

The opening of Phantom Hourglass, 3D on both screens, 1,800 frames with
no input, interpreted: 1x 6.3 ms a frame, 3x 10.6, 4x 14.1 (eight
threads). Recompiled, frames 300 to 1,800: 5.8, 10.7 and 15.0, so 4x
leaves little of a 16.7 ms frame there. Its state is the same at every
scale (`E92B61EA5783924F` after 1,800 frames, the value from before).

**Not done.** A capture used as a texture, scaled, flipped or blended as it
is taken is the console's own pixels. A sprite pixel whose finer pixel is
empty shows the backgrounds, not a sprite underneath. Texture filtering:
textures are point-sampled, as on the console, so a larger picture shows
texels as squares. Edge marking's outlines are one larger pixel wide. The
larger frame is drawn in step with the machine, which waits for it; it
could be drawn while the machine runs on. No hub setting: it is the game's
own.

## Behaviour pinned by the tests

What the ARM9 does that the ARM7 does not, each checked in
`tests/nds/test_nds_cpu.c` with the ARM7 alongside:

- LDM/STM. An empty list transfers nothing but moves the base by 0x40. STM
  always stores the old base. LDM with the base in the list writes the new
  base back over the loaded value unless the base is the last of several
  registers. Writeback goes to the current mode's base register even when
  the S bit transfers the user bank. (GBATEK; rockwrestler.)
- A word loaded into R15 by LDR, LDM or POP selects the instruction set by
  its bit 0, unless CP15 control bit 15 is set. Data-processing writes to
  R15 never do. R15 is always held aligned.
- LDRH and LDRSH from an odd address read the aligned halfword, with no
  rotation and no byte sign-extension. LDR rotates on both processors.
- Multiplies leave C alone.
- MSR cannot change T.
- Encodings ARMv6 later used (UMAAL, MLS, LDREX/STREX, MOVW, BXJ) are
  undefined; the ARM7TDMI decodes them as the multiply, swap or MSR they
  resemble.
- BKPT enters abort mode through the prefetch-abort vector with LR = the
  instruction + 4 in either state.
- The vectors are at `0xFFFF0000` or `0` as CP15 control bit 13 says.

## Known deviations

No vector fails. Four choices rest on the architecture manual or on melonDS
alone, with no hardware test behind them; games do not depend on any of
them:

| Case | What we do | Why |
|---|---|---|
| ARM condition `1111` other than BLX and PLD | Undefined trap | ARM ARM (and QEMU). melonDS skips the instruction. |
| Thumb conditional branch, condition 14 | Undefined trap on the ARM9, taken on the ARM7 | ARM ARM for the ARM9; the ARM7TDMI vectors for the ARM7. melonDS takes it on both. |
| LDRD/STRD with an odd Rd | Undefined trap | ARM ARM. melonDS uses Rd - 1. |
| R15 as Rn in a register-shifted data-processing instruction | Reads PC + 8 on the ARM9, PC + 12 on the ARM7 | melonDS for the ARM9; the ARM7TDMI vectors for the ARM7. |

Where the machine follows one source against another, or is known to be
wrong:

| Case | What we do | Why |
|---|---|---|
| What ends the ARM9's halt (CP15 wait for interrupt) | IE & IF with CPSR.I clear, whatever IME says | The table in BlocksDS's `tests/system/irq_handling`, taken on a console. melonDS and GBATEK key it on IME. |
| VRAMCNT_A to I, WRAMCNT read by the ARM9 | Read back what was written | libnds reads them; BlocksDS's texture tests pass only so. GBATEK marks them write-only. |
| CLZ and the other ARMv5 encodings on the ARM7 | Run as the ARM7TDMI's nearest instruction (CLZ as MSR) | Wrong: arm7wrestler's notes say they trap. Seen only on that ROM's V5TE page. |
| Debug output | The I/O ports at 0x04FFFA10 to 1C only | The opcode form (`mov r12, r12` and inline text) is not printed. |
| ARM9 access times | An average per kind of memory, no cache model | P8. |
