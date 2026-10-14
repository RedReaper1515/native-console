# Adding a console

This is how a new console family gets into the hub. It generalises what was
done for the first two families the hub supported:

- **Game Boy / Game Boy Color** came from gb-recompiled. The recompiler and
  runtime were already there. The hub was built around them.
- **Game Boy Advance** was added from scratch between 2026-09-10 and
  2026-09-11. It went from no code to a commercial game playing in the hub,
  in four pieces of work: the recompiler, runtime and hub backend; profiling
  before recompiling; audio; and the cartridge RTC. Its plan and results
  are in [GBA-PLAN.md](GBA-PLAN.md).

After those came the platform work: Linux/SteamOS, macOS and Compile for
export. Those paths were written against a contract that both consoles
already met. A new console that meets the same contract gets Linux, SteamOS,
macOS and export builds with no changes to the export code.

Three more families followed this playbook:

- **SNES** ([SNES-PLAN.md](SNES-PLAN.md)) added a second CPU and cartridge
  coprocessors (SA-1).
- **GameCube** ([GC-PLAN.md](GC-PLAN.md)) was
  the first disc-based console with a 3D GPU. It brought the lessons in
  [From GameCube](#from-gamecube), a variation on the contract for disc
  data, and most of what P8 now says about measuring.
- **NES** ([NES-PLAN.md](NES-PLAN.md)) was the first where nearly every
  cartridge switches banks under the running code. It brought the lessons
  in [From NES](#from-nes), among them how to key recompiled code so a
  wrong guess about banking cannot run the wrong bytes.
- **Wii** ([WII-PLAN.md](WII-PLAN.md)) was the first console added as a
  second member of a family that already existed, and the first whose
  games talk to an operating system rather than to hardware. It brought
  the lessons in [From Wii](#from-wii). It is not supported yet: no retail
  game has been through its gates.

Throughout this document `<f>` means the new family id: short and lower
case, such as `gba`. `<F>` is its upper-case form.

## What "supported" means

A console is supported when all of these hold. Until then its hub backend
stays behind `supported() == false`. Its ROMs are still catalogued.

1. The hub imports a ROM of that console, builds a native executable, plays
   it, and captures a cover. The battery save survives a rebuild. The game
   has an in-game overlay menu (see [Overlay menu](#overlay-menu)).
2. **Compile for export** produces a working Windows zip, Linux / SteamOS
   tarball and macOS kit for it.
3. The evidence below exists and is recorded in `docs/<F>-PLAN.md`:
   - per-instruction CPU test vectors pass
   - the hardware test ROMs give the same verdict recompiled as interpreted
   - `--differential` lockstep runs clean
   - at least one commercial game that the user owns plays in the hub

"Supported" is not a claim that the whole catalogue is compatible. The
evidence layers in [AGENTS.md](../AGENTS.md#accuracy-workflow) still apply.
Differential mode is not an independent hardware oracle.

## The contract

Everything outside the console's own directories talks to it through this
contract. Meet it exactly, and the hub pipeline, exports, the platform
scripts and CI work without special cases.

### Recompiler (`<f>recomp`)

- `<f>recomp ROM -o DIR --name ID [options]` writes a CMake project to `DIR`
  that builds on its own. `ID` is a C identifier and is used for the
  executable name and file prefixes. (`gbrecomp` spells the option
  `--output-prefix`; new recompilers use `--name`, as `gbarecomp` does.)
- The project contains a snapshot of the runtime in `DIR/runtime-<f>/`. That
  folder name is what the backend's `runtime_folder()` returns. The
  recompiler looks for the runtime sources in `--runtime DIR`, then
  `$<F>RECOMP_RUNTIME_DIR`, then `runtime-<f>/` next to its executable (a
  release archive), two folders up (`build/bin` in a checkout), and last
  the directory compiled in at build time (`<F>RECOMP_DEFAULT_RUNTIME_DIR`).
- `<f>recomp --version-json` prints `{"schema":"<f>recomp.version",
  "schema_version":1,"version":…,"abis":{…},"features":[…]}`. The release
  manifest records it.
- Exit non-zero on any failure. The hub, export and CI all rely on exit
  codes.
- Anything the recompiler needs besides the ROM, such as a profile or a
  trace, is an explicit input. It lives in the data directory the hub passes
  to `generate_steps()`, never in the wiped work tree.

### Generated project

- Portable C11 (C++20 only if unavoidable). It must compile with MSVC, gcc 14
  (the Steam Runtime SDK) and Apple clang for arm64 and x86_64.
- The target is named `ID` and builds to `build/ID` (`build/ID.exe` on
  Windows).
- It accepts these CMake options:
  - `-DCMAKE_BUILD_TYPE`
  - `-DSDL2_DIR`
  - `-DGBRECOMP_GENERATED_COMPILE_JOBS`, which exports always pass; a
    project that has no use for it can ignore it
  - `-DNATIVE_CONSOLE_MACOS_BUNDLE=ON`

  Without SDL2 it still builds a headless executable.
- The ROM is compiled into the executable. `gbarecomp` uses an `RCDATA`
  resource on Windows and `.incbin` elsewhere, because a multi-megabyte C
  array compiles slowly. No ROM, BIOS or data file ships next to the game.

  **Disc-based consoles** are the one exception. The code is still
  embedded, but gigabytes of disc content can't be: MSVC, `ld` and a
  release zip can't carry a 1.4 GB `.incbin`. GameCube embeds `main.dol`
  and every `.rel`, and packs the rest of the disc into one `ID.gcdata`
  file next to the executable. The backend's `install_files()` lists that
  file, so every export path carries it. Decide this with the user in P0,
  and tell them what size to expect: Shrek 2's executable is about 80 MB
  from a 3 MB DOL. That's generated code, not the game compressed.
- The only runtime dependency is SDL2. On Windows, a post-build step copies
  `SDL2.dll` next to the executable. On Linux, link `m` explicitly, and
  `Threads` wherever threads are used.
- The runtime carries `cmake/NativeConsoleMacBundle.cmake`, `Info.plist.in`
  and `make_macos_app.sh`. These are identical copies of the files in
  `runtime/cmake/`, and CTest compares them.

### Game executable

- **Saves**: `ID.sav` is written next to the executable. Inside a macOS app
  it goes to `~/Library/Application Support/gbrecompiled/ID/`. `--save-dir
  DIR` overrides both. The backend lists every file that must survive a
  rebuild in `is_persistence_file()`.
- **Headless runs** are deterministic and never write the player's save:
  - they take a frame count and scripted input
  - they dump a frame as a PPM (Netpbm) file for cover capture
  - they dump machine state for A/B comparison
  - clocks use emulated time, not the host clock

  The hub's cover capture, the platform checks and CI depend on this.
- **Differential mode**: `--differential` runs the generated code and the
  interpreter in lockstep. They are compared after every native slice.
- **Frontend**: keyboard and controller input, audio, fullscreen,
  screenshots, a mute option and a frame limit are expected. The existing
  runtimes already spell these flags differently: GB uses `--headless
  --limit-frames` and `--no-audio`, GBA uses `--frames` and `--mute`. Pick
  one existing set (see [RUNTIME.md](RUNTIME.md) and [GBA.md](GBA.md))
  rather than inventing a third. Whatever the spelling, the mute flag
  silences that run only: it must not set the saved Mute setting, or the
  next settings change writes it to the player's preferences file. The
  frontend flushes stdout and stderr before every `SDL_Quit()` (`quit_sdl()`
  in each frontend): on Windows a program that quits within a moment of
  `SDL_Init(SDL_INIT_GAMECONTROLLER)` otherwise loses its buffered output
  now and then, with the exit code intact, and scripts read the menu
  self-test's verdict and the screenshot lines from stdout.
- **Overlay menu**: every console's games have one. See
  [Overlay menu](#overlay-menu).
- **No copyrighted firmware.** If the console has a BIOS, emulate it in C at
  high level (HLE), with the vectors at the real addresses so open-bus reads
  match. A user's own dump may be accepted as an option (`--bios`), but it
  is never required and never shipped.

### Overlay menu

Every console's games have an in-game overlay menu. Players get the same
controls whichever console a game came from. The Game Boy runtime's settings
menu is the reference: see the controls table and the settings menu in
[RUNTIME.md](RUNTIME.md). It is built with the Dear ImGui copy vendored in
`runtime/vendor/imgui`. The GBA menu is in progress; the SNES frontend
(`runtime-snes/platform/snes_sdl.cpp`) was adapted from it and has a
self-test (`--menu-selftest`, CTest `snes_menu_selftest`) that drives the
menu with synthetic keyboard and controller events, the model for the P5
menu gate.

ImGui is the way to build it, not the point of it. A runtime that cannot
take ImGui still owes the player the same menu, drawn whatever way it can
draw. What matters is the contract below, and that
`--menu-selftest` covers it — including the save, which must put the
bindings back as it found them rather than skip writing them. A self-test
that skips the save does not test the save.

- **Opening it.** Escape or F10 on a keyboard. On a controller, a button
  that works when there is no keyboard: a game launched from Steam's Game
  Mode on a Steam Deck has only the controller, and Steam keeps the Guide
  button for itself. The GB runtime also opens the menu with Guide/Home
  or L3.
- **While it is open**, the game pauses and no input reaches the guest.
- **What it offers.** It remaps the keyboard and controller, and it controls
  volume and mute, speed and fast forward, fullscreen and scaling, and
  savestate slots where the runtime has savestates. **Show FPS Counter**
  (F9 by default) puts one line in the window's corner: emulated frames per
  second and the game's speed as a share of the console's own rate.
- **Where settings go.** Preferences are stored in SDL's per-application
  preference directory, not next to the game. If the menu writes files next
  to the executable (savestates, a UI layout such as `imgui.ini`), the
  backend's `is_persistence_file()` lists them, so a rebuild keeps them.
- **Headless runs** never open the menu and never write its files.

### Hub backend

`ConsoleBackend` in [hub_consoles.h](../hub/src/hub_consoles.h):

| Method | What it does | GBA example |
|---|---|---|
| `family_id`, `family_name`, `extensions` | Identity; lower-case extensions with a leading dot | `"gba"`, `{".gba"}` |
| `file_names` | Optional: the exact lower-case names the family's game goes by when it has none. Imports, the file dialog and `backend_for_file()` take either. | `{"game"}` |
| `probe` | Validates the header and fills `RomInfo` (console id, header title, notes) | Checks the fixed byte `0x96` at `0xB2` |
| `recompiler_name` | The recompiler's executable name. Settings finds, stores, shows and checks it by this name alone. | `"gbarecomp"` |
| `generate_steps` | Ordered commands that turn the ROM into `source_dir`. `data_dir` keeps anything that must outlive the work tree (`trace_files()` lists its traces). Used by library builds and exports. | `--profile`, then `--trace` |
| `runtime_folder` | Runtime folder inside the generated project | `"runtime-gba"` |
| `cmake_args`, `requires_sdl2` | Optional: extra `-D` arguments, and whether configuring needs SDL2 | GB: `GBRECOMP_GENERATED_COMPILE_JOBS`, SDL2 required |
| `prepare_build` | Writes `build.cmd` / `build.sh`: generate steps, CMake, Ninja, copy `SDL2.dll`. The default serves every family that compiles something; override it only if yours does not: such a family writes its own script and reports no CMake or MSVC in `toolchain_problems`. | the default |
| `link_name`, `link_port` | Non-empty when the family's games can be played together: the heading the game's page shows, and the port a host listens on. The runtime takes `--player N`, `--link-host`, `--link-local` and `--link-join HOST`. | `"Link cable (multiplayer, trading)"`, 47400 |
| `two_player_args` | Extra arguments for window 1 and window 2 of a two-window launch, where the two share one set of devices — which window gets a controller only one can have, where each window sits on the screen, a second log file | player 2 gets its own log |
| `launch_args` | Arguments appended to every launch | none |
| `install_files` | Extra files installed next to the executable, hard-linked where the volume allows (a GameCube disc). Content too large to copy when the link cannot be made is left where it is and its folder recorded by `prepare_build` instead. | none (the ROM is embedded) |
| `is_persistence_file` | Save files kept across rebuilds | `ID.sav` |
| `supports_cover_capture`, `cover_capture_args` | Headless run that dumps frame N to `<prefix>_<NNNNN>.ppm` | `--frames N --dump-frame … --quiet` |

## Phases

Each phase ends with a gate. A phase is done when its gate passes, not when
its code is written. Record gate results with dates in the status table of
`docs/<F>-PLAN.md`. GBA reached P6 in two days because every phase had a
test gate from the start. Do not skip gates to go faster.

Work in a git worktree on a `feature/<f>-backend` branch cut from `main`.
Open the first PR once P4 or P6 passes. After that, open one PR per feature,
as GBA did.

### P0: research, licenses, test data

- Write `docs/<F>-PLAN.md` from the [template](#plan-template). Write its
  ground rules, layout, phases and a table of hardware nuances. Map every
  nuance to the test that covers it.
- Check the license of every candidate source before reading its code:
  recompilers, disassemblers, test suites, reference emulators and
  replacement BIOSes. Use `gh api repos/OWNER/REPO/license`. The rule:
  - MIT, BSD, Apache-2.0, zlib or CC0: may be copied, recorded in
    [THIRD_PARTY.md](../THIRD_PARTY.md)
  - MPL: run as a separate oracle program, never linked
  - GPL: run as a tool or read as a behaviour reference only
  - no license: run as a tool only, never vendored
  - noncommercial: read for ideas only

  Put the results in the plan's "What we reuse" table.
- Add `tools/fetch_<f>_tests.py`. It downloads each suite at a pinned commit
  into `roms/<f>/<suite>/` (gitignored), verifies the commit, and writes
  `roms/<f>/MANIFEST.json` with each suite's source and license.
- Add the console's ROM extensions to `.gitignore`.
- **Gate:** the fetch script runs clean twice, and the second run does
  nothing.

### P1: CPU core

- **One source of instruction semantics.** A decode classifier is shared by
  the interpreter, analyzer and emitter. Instruction handlers are
  force-inlined. The interpreter calls them after decoding. Generated code
  calls them with constant opcodes, so the decoding folds away. The emitter
  only sequences handlers. This is how the GB lesson (emitter and
  interpreter drifting apart) was avoided on GBA.
- **Guest state is explicit.** Registers and flags live in a context
  struct. Every memory access goes through the bus. No guest value is held
  in a C local across an instruction that can fault, branch or touch I/O.
  This holds most C-level hazards (width, overflow, aliasing, endianness)
  away by construction.
- Add a per-instruction vector harness in `tests/<f>/`. It checks registers,
  flags, banked state, and the ordered bus transactions (address, size,
  data) including instruction fetches. Read the vector files directly.
  SingleStepTests has vector sets for many CPUs; check which cover this one.
- **Gate:** 100 % of vectors pass in Release (`/O2`), Debug and
  AddressSanitizer builds, and a UBSan build with clang or gcc. Any
  deviation is listed in the plan with its case and reason.

### P2: machine model and headless runner

- Memory map, mirrors, access widths and open bus. The I/O register file is
  described as data (address, width, read mask, write mask, side-effect
  hook), not written by hand per register. Add timers, DMA, interrupts,
  input and the HLE BIOS.
- Add `<f>_run`, a headless interpreter runner. It runs N frames, dumps
  frame and memory, and reads each test suite's verdict the way that suite
  reports it.
- Add `tools/<f>/run_<f>_tests.py` and a baseline file. The CTest test
  `<f>_test_roms_baseline` fails if any ROM in the baseline stops passing.
- **Gate:** the core CPU and memory suites pass in the interpreter. Every
  other failing ROM is listed with the phase that owns it.

### P3: video

- Build the renderer, with per-scanline latching where the hardware latches.
- **Gate:** the video test ROMs pass. Frames match the reference emulator
  (`tools/<f>/compare_frame.py`). Photo-based reference images are checked
  by eye, and the plan says so.
- **3D consoles.** Build the software rasteriser first, because it's the
  one that can be checked against hardware tests (`gxtest` for GameCube).
  A GPU renderer comes after it, must match it, and stays selectable.
  Neither one is always faster. The GameCube OpenGL path was faster until a
  TEV fix brought back blended foliage, and after that the software path
  won on Shrek 2. Measure both on real scenes before picking the default.
- **Test the whole encoding space.** A register field that selects among N
  things gets a test for all N. The GameCube TEV konstant selectors were
  tested for 8 of 16, and 14 of 16 were wrong. That broke water, foliage
  layers and glows in the first game.

### P4: recompiler

- Build `<f>recomp`:
  - **Analyzer** — entry points come from the vectors, the interrupt
    handler, symbol files, traces and profiles.
  - **Emitter** — one C block per guest instruction, calling the shared
    handlers.
  - **Dispatcher** — maps a guest address (and mode) to a native function
    and falls back to the interpreter for anything not compiled.
  - **Resume labels** — native code can be re-entered at function entries,
    branch targets and return sites.
- Generated code charges the same cycles as the interpreter. It yields to
  the run loop when an event is due. I/O reads catch up on due events
  first.
- Code in RAM, self-modifying code and missed indirect targets are
  interpreted. They may instead be compiled from a trace and checked
  against live RAM before running natively.
- Add `--differential` (registers and cycle counter after every native
  slice, memory every frame). Add `tools/<f>/recomp_suite.py` and a CTest
  test `<f>_recomp_*` on a subset.
- **Gate:** every test ROM gives the same verdict recompiled as interpreted,
  and runs clean in `--differential`. For GBA: 138/138 and 138/138.

### P5: audio, saves, frontend

- Build the audio unit. Add a unit test on the mixer's output stage. GBA's
  clicking had two causes: a DC offset in the mixer and an audio queue that
  ran dry. Add every save type the cartridges use, and cartridge extras such
  as an RTC or a mapper chip.
- Build the SDL2 frontend: window, keyboard, controller and audio. Keep the
  audio queue about 80 ms ahead of the device, with ±0.5 % frame pacing.
  Add `--audio-stats`. Add `<f>_play`, which plays a ROM with the
  interpreter alone.
- Build the [overlay menu](#overlay-menu).
- **Save states**, if the menu offers them, need these from the start:
  - a format version, and a layout fingerprint computed for that version
  - host pointers scrubbed on save and restored on load; every new pointer
    field in the machine struct goes into that list
  - an exact-resume test: save at frame K, load, run on, and compare with a
    straight run, recompiled and interpreted crossed

  They also make differential runs cheap. GameCube: 19 s from a state
  against 325 s from boot.
- **Gate:**
  - the save-type test ROMs pass
  - a 30-second windowed run reports no audio underruns
  - the protocol tests for cartridge extras pass
  - the overlay menu opens from the keyboard and from a controller alone,
    pauses the game, and its settings survive a restart

### P6: hub integration and the first game

- Add the hub backend and settings. See the [checklist](#checklist-files-to-touch).
- If static analysis misses code that games reach through RAM or function
  pointers, add a profiling generate step. It plays the game with the
  interpreter under a built-in input script and records entry points. The
  trace goes to `<game_dir>/profile/`, and the recompile step uses it.
- Cover capture runs headless and never touches the real save.
- The backend's `export_problem` refuses every target nobody has exported
  one of its games for and run there: start with
  `untested_export_problem(family_name(), target_id, {})`, add `"windows"`
  once the P6 game's Windows export unpacks and runs, and `"linux"` and
  `"macos"` at P7. Update the export tables in the README and
  `hub/README.md` with it.
- **Check what the window shows.** Headless dumps wait for pending copies
  to finish, so they can look right while the window doesn't. On GameCube
  the window went black and then froze after a cutscene skip, and every
  gate still passed. Log presented frames (`--shown-log` on GameCube) and
  gate on them too.
- **Gate:** a commercial game the user owns is taken through the hub:
  - it imports, builds, plays and gets a cover
  - its save, and any files its overlay menu writes, survive a rebuild
  - `--differential` is clean over a scripted run of at least 6,000 frames
  - the share of native code is reported (`--coverage`)

  Minish Cap: 99.9 % native.

### P7: platforms and export

No export or hub pipeline code should change. If it has to, the contract
was not met. Fix the console instead.

- Add the console to `tools/consoles.sh`, the table both
  `tools/steamos/build_in_sdk.sh` and `tools/macos/build_macos_app.sh` read:
  - the extension mapping (`console_for_rom`)
  - the recompiler (the Ninja target), how it takes the game id, the runtime
    folder, and whether it takes `--trace` (`console_info`)

  and add the `-Console` value to `tools/steamos/build_steamos.ps1`.
- In [.github/workflows/macos.yml](../.github/workflows/macos.yml) (opt-in:
  it runs on a pull request labelled `run-ci`, or from Actions by hand):
  - add the new directories to the path filters
  - build an app from an MIT-licensed test ROM, run it, and check the save
    location and signature
  - add the ROM to the `windows-export` job

  User ROMs never go to CI.
- Add the console to the release:
  - a `CONSOLES` row in [tools/create_distribution.py](../tools/create_distribution.py):
    family id, recompiler name, runtime folder, and the runtime files a
    generated project cannot build without
  - a recipe in `SMOKES` in [tests/test_release_bundle.py](../tests/test_release_bundle.py):
    recompile a repository-owned or MIT test ROM with the shipped
    recompiler, build the project, and run it with a pass/fail check
  - in [.github/workflows/release.yml](../.github/workflows/release.yml),
    fetch that ROM in each platform job if it is not generated

  The recompiler must find its runtime folder next to its executable and
  answer `--version-json`, because the archive is used from wherever it is
  extracted.
- **A/B determinism.** Build the same game on Windows (MSVC) and in the
  sniper SDK (gcc 14). Run both with the same scripted input and an empty
  save directory. The `--dump-state` output and the frames must be
  byte-identical. Then run the Linux build in the SDK, in the sniper
  runtime, and on a plain distribution.
- **Gate:** the A/B check is identical. Linux CTest passes. The macOS CI job
  is green. The hub's export of the P6 game works for all three targets,
  and the backend's `export_problem` lets all three through.

### P8: accuracy and speed

- Timing test ROMs, then performance, following the GB `NL*` method:
  measure first, keep only changes whose state, frame and PCM output is
  identical, and record rejected experiments too.
- Measure generated-code size per guest instruction (MSVC `/FAcs` listing)
  before and after every emitter change.
- Profile each layer before optimising it: guest code, the machine's own
  runtime, and drawing. On GameCube, guest code turned out flat (the
  hottest block was 0.6 % of the run), so no single block was worth fixing.
  The lever was the emitter: keeping guest registers in C locals across a
  block made guest code about twice as fast and the executable 40 %
  smaller.
- **No fixes for individual games.** When a game shows something wrong,
  the bug is in how the runtime implements the console. Find it with a
  probe and the hardware tests, and fix it there. A list of per-game
  patches is the failure mode.
- Follow the measuring rules in [From GameCube](#from-gamecube).

## Checklist: files to touch

New directories and files:

| Path | Contents |
|---|---|
| `recompiler-<f>/` | `CMakeLists.txt` (`<f>recomp_core`, `<f>recomp`, `<F>RECOMP_DEFAULT_RUNTIME_DIR`), `src/` rom, analyzer, emitter, CLI |
| `runtime-<f>/` | `include/`, `src/`, `platform/` (SDL frontend and overlay menu), `tools/` (`<f>_run`, `<f>_play`), `cmake/` (the three macOS files) |
| `tests/<f>/` | `CMakeLists.txt`, vector harness, device unit tests, ROM baseline. Name every test `<f>_*` so `ctest -R <f>` selects them. Register tests that need fetched data only when `roms/<f>/` exists, and print a status message when it does not. Runners fail closed. |
| `tools/<f>/` | `run_<f>_tests.py`, `recomp_suite.py`, frame tools |
| `tools/fetch_<f>_tests.py` | Pinned test-data downloads and `MANIFEST.json` |
| `hub/src/console_<f>.cpp` | The `ConsoleBackend` implementation and `make_<f>_backend()` |
| `<F>.md`, `docs/<F>-PLAN.md` | User guide (status, usage, tests, what is not done), and the plan (phases, gates, results) |

Existing files:

| File | Change |
|---|---|
| `CMakeLists.txt` | `add_subdirectory(runtime-<f>)`, `add_subdirectory(recompiler-<f>)`, and `tests/<f>` under `BUILD_TESTS` |
| `tests/CMakeLists.txt` | Add `runtime-<f>` to the macOS bundle copy comparison |
| `.gitignore` | ROM extensions |
| `hub/src/hub_consoles.cpp` | Declare `make_<f>_backend()`, push it in `backends()`, add the console ids to `all_consoles()` |
| `hub/CMakeLists.txt` | `src/console_<f>.cpp` |
| `hub/src/hub_app.cpp` | The console colour in `console_color()` (the recompiler's Settings field, detection and checks come from `recompiler_name()`) |
| `tools/consoles.sh`, `tools/steamos/build_steamos.ps1` | One entry in each table function; the `-Console` value set (see P7) |
| `.github/workflows/macos.yml` | Path filters, a test-ROM app, the export job |
| `tools/create_distribution.py`, `tests/test_release_bundle.py`, `.github/workflows/release.yml` | A `CONSOLES` row, a `SMOKES` recipe, and its test ROM (see P7) |
| `README.md`, `hub/README.md`, `docs/LINUX.md`, `docs/MACOS.md` | Supported consoles, the `--console` values, build targets |
| `AGENTS.md`, `TODO.md`, `THIRD_PARTY.md` | Documentation map, backlog, ported code |

Hub builds made with a wrapper script, or from a separate clean checkout of
`main`, also need the new recompiler added to that script's `ninja` target
list. The hub only finds recompilers that were built.

## Before the third console

Adding GBA left some duplication. It was removed with the SNES backend, the
third family:

- **Done.** One build-script writer for every backend
  (`ConsoleBackend::prepare_build` in `hub_consoles.cpp`); a backend supplies
  `generate_steps()` and, if needed, `cmake_args()` and `requires_sdl2()`.
- **Done.** Toolchain validation is per backend
  (`ConsoleBackend::toolchain_problems`, on top of
  `Settings::shared_toolchain_problems`); `Settings::validate()` gathers every
  console's.
- **Done.** Recompiler paths are one map from family id to path
  (`Settings::recompilers`), filled by auto-detection from each backend's
  `recompiler_name()` and shown on the Settings page from it. Old
  `gbrecomp_exe`/`gbarecomp_exe` keys are still read and written.
- **Done.** The SteamOS and macOS scripts read one table, `tools/consoles.sh`.
- **Done.** Releases package every console's recompiler and runtime from
  the `CONSOLES` table in `tools/create_distribution.py` (PR #15).

## Lessons from earlier consoles

### From GB, GBA and SNES

- **Headless runs must not write saves.** The GB runtime loads the save
  next to the executable and writes it back on exit, headless too. A
  scripted check once ran against a player's real save that way. Cover
  capture uses `--save-dir <game_dir>/scratch-save`. The GBA runner never
  writes saves.
- **Scripted timelines depend on the save.** A fresh save shifts events by
  hundreds of frames. Prime or empty the save directory the same way on
  both sides of any comparison.
- **Windows path length.** MSVC fails beyond 260 characters, and generated
  object paths repeat the game id. Test hub builds and exports with a short
  data directory. Deep scratch folders fail for long game ids.
- **Compiler bugs are real.** MSVC 19.51 `/O2` with AVX2 miscompiled a loop
  of `h[k+i] = x >> (8*i)` stores. Write little-endian fields with a helper
  such as `put_le32`.
- **Linux portability** needed explicit `m` and `Threads` links, missing
  standard includes (`<algorithm>`, `<stdlib.h>`), and CMake arguments
  passed as one token (`--cmake-arg=-D...`).
- **Static analysis misses code.** Code copied to RAM and code reached only
  through function pointers ran interpreted until profiling was added. Plan
  a profile or trace input from P4.
- **Audio problems show up as clicks.** Check the mixer's DC offset and the
  queue depth, and measure them (`--dump-audio`, `--audio-stats`,
  `SDL_AUDIODRIVER=disk`) rather than listening.
- **Fail closed.** Missing test data, a timeout, an empty selection or a
  missing state dump is a failure. So is a run that writes no state.
- **Stale builds cause false conclusions.** Regenerate and rebuild the
  generated project after runtime changes. Delete old state files between
  runs of a sweep.
- **Keep the hub build clean.** Build the hub the user plays with from a
  clean checkout of `main`, not from a feature branch. Close the GUI hub
  before running `--batch` against the same data directory. A running hub
  locks its own executable.
- **Stacked PRs:** deleting a parent PR's branch closes its child. Retarget
  the child to `main` first.
- **Guessed code snowballs on big ROMs.** Static exploration that follows
  calls into guessed code decoded all of a 4 MiB SNES ROM, in every CPU mode
  (15 million "instructions"), because data decoded as code calls into more
  data. Accept guessed code region by region, reject regions with opcodes
  real code does not use or that straddle accepted code, drop a rejected
  region's descendants, and bound the depth of guessing when a trace exists
  (SNES-PLAN.md, P4).
- **Yields must land on resume points.** When native code can yield after any
  instruction (events, a second CPU catching up), every compiled instruction
  must be a dispatcher entry, or execution falls back to the interpreter at
  every yield. The SA-1's wait loop ran 15 % interpreted until this was fixed.
- **Two CPUs on a cartridge stay deterministic** if the lagging one is caught
  up at the same bus cycles whether the other is native or interpreted, and
  always stops at the first instruction boundary past its target.
- **Keep scratch paths short.** A suite of long test-ROM names under a deep
  temporary directory broke CMake's compiler check with MSVC's 260-character
  limit; name per-ROM directories by a short stem and a hash.

### From GameCube

Details and numbers are in [GC-PLAN.md](GC-PLAN.md).

- **No register lookups by name on hot paths.** The GameCube machine looked
  up an I/O register by name (`strcmp`, then a linear scan) between
  instructions. That was three quarters of all run time. Resolving the slot
  once and keeping its index made the machine 3.8x faster. Cache it as an
  index, and reset it to an explicit "missing" value rather than 0, because
  0 is a valid index. Measure by stubbing the suspect function out and
  timing a fixed number of frames.
- **A per-pixel probe finds rendering bugs.** For one pixel chosen by
  environment variable, print the whole pixel pipeline right before the
  framebuffer write: every TEV stage's inputs, texture fetches, alpha
  compare, blend state and the value underneath. That's how the missing
  water was traced to the konstant selectors. Gotchas:
  - read the environment once, not per pixel
  - with a graphics thread, the draw runs on a shadow machine whose field
    counter doesn't advance, so filter on something else
  - helper threads make the output order random; run single-threaded for
    ordered output
- **Measuring rules**, each learned by getting it wrong:
  - Find where time goes in a pixel loop by skipping a stage and timing the
    run (ablation). Timers inside the loop perturb it.
  - Never put a shared counter on a helper-thread hot path. One triangle
    counter cost 5 % in cache-line traffic. Count at the flush.
  - A probe can create the effect it measures. A threshold "win" of 183 to
    141 slow fields was the probe's own contention, and disappeared when
    the probe did.
  - Judge by total wall time. Machine-thread time per field shifts when
    work moves to another thread.
  - Five runs of one binary land within 1 %, so a 2 % change is real.
  - Measure the GPU with timestamp queries before theorising about GL
    costs. All three CPU-side theories about the OpenGL path were wrong.
  - Measure the installed executable, at the path the hub uses. The NVIDIA
    driver applies different settings to an unfamiliar executable path,
    and reads back 5 to 8 times slower.
- **Sampling profilers and recompiled code.** Mapping a sample to the
  nearest block entry at or below it can't tell where a function ends. The
  runtime and every system DLL landed on the last block in the table and
  read as 20 % of the run. Count samples further past an entry than a block
  can reach separately. The linker also folds identical blocks, so trust the
  split between layers more than any per-block line.
- **Tail latency and throughput are different levers.** On GameCube, hitches
  followed triangle count, while average cost followed pixels.
- **A local stand-in for macOS CI.** macOS CI is opt-in. A clang-only error
  (an array parameter of incomplete type) reached it once. Running
  devkitPPC gcc with `-fsyntax-only -std=gnu99` (C++17 for the recompiler
  and platform code) catches that class locally.
- **Scripted runs must not touch the player's save.** Copy the card or save
  first: GameCube's `--auto-input` starts a New Game and overwrites slot A.
  State hashes include the card's path, so compare runs that use the same
  save-directory path.

### From NES

Details are in [NES-PLAN.md](NES-PLAN.md).

- **Key code by its bytes, not only its address.** On a banked cartridge
  the same address holds different code over time. A recompiled entry is
  keyed by the address and the ROM page mapped there, and the run loop
  builds the key from the cartridge's real page table. Then a compiled
  entry can never run against the wrong code, and everything the analyzer
  guesses about banking only decides how much gets compiled. Keep a
  function inside one mapping unit (an 8 KiB page), and send generated code
  back to the run loop after a write that changes the mapping.
- **Guessing is cheap when a wrong guess is harmless.** With such keys the
  analyzer can try a call target in every bank it could be in and keep the
  ones that decode as plausible code. Reject on what real code never does
  (unofficial opcodes, jumps into I/O space), not on what it rarely does.
- **Look for jump tables where the pointer is filled, not where it is
  used.** Static exploration of Final Fantasy stopped at 14,000
  instructions until tables were read. The table's address is next to the
  store into the pointer; the `JMP (ptr)` is often in another routine.
- **Split a machine at link-time seams.** The CPU core, the PPU and the APU
  are libraries that call functions the final program provides (the bus,
  the cartridge's side of the PPU bus, the DMC's sample fetch). Each was
  written and tested alone, at the same time, and the machine around them
  ran nestest and blargg's CPU suite on its first build. Write the seam's
  contract down before the parts are written: which dot a register access
  sees, what cycle count an access carries, and that an event horizon
  ("nothing changes on its own before N") may be short but never long.
- **Catch devices up; do not step them.** With exact event horizons the
  PPU and APU run only when the CPU touches them or when they could change
  an interrupt line, and every edge still gets the time it happened. Where
  inside a CPU cycle an access lands is a handful of constants in one
  header, tuned against the timing test ROMs.
- **Write the recompiler's own test ROM.** Hardware suites test the
  hardware. A ROM assembled in the repository tests what a recompiler gets
  wrong: a bank switched out under the running routine, an instruction
  that straddles two banks, the code after a call that never returns. It
  also found two machine bugs the hardware suites pass over. Take expected
  values from the hardware documentation, and run the ROM on an unrelated
  emulator before trusting it.
- **Independent evidence can disagree.** The per-instruction vectors and a
  hardware-verified test ROM differ on one unstable opcode. Pick the rule,
  make the other harness count the difference as a deviation against that
  rule, and write down which chip each source describes.

### From Wii

Details are in [WII-PLAN.md](WII-PLAN.md).

- **A console that contains an earlier one is a mode of its runtime, not
  a copy of it.** The Wii shares the GameCube's processor core, graphics,
  renderers, sound path, recompiler and frontend, and adds files of its
  own. A copy would have been forty thousand lines to keep in step by
  hand. Three things made the mode cheap:
  - what differs between the consoles in every loop (memory size, clock
    rates) is chosen once for the process, before the first machine is
    made, rather than carried in every structure;
  - the new console's state hangs off a pointer the old machine structure
    already had, so that structure's layout - which save states check -
    did not change and old states still load;
  - addresses a register holds become offsets into one block of memory
    through one helper, which on the old console is the mask it always was.
- **Hold the old console to itself while you do it.** Every shared-code
  change was followed by the old console's tests, and at the end the same
  commercial game was built from `main` and from the branch and run over
  the same scripted input: identical state hash, same time. That is the
  evidence that the old console is untouched; "the tests pass" is not.
- **Build the old console's test programs for the new one.** The GameCube
  programs compiled for the Wii have to give the same verdicts, and the
  drawing ones the same frame, byte for byte. They reach the same hardware
  through the new console's start-up, memory map and clocks, so a
  difference names something the new half got wrong. It cost one script.
- **Build a disc that boots.** No retail game is in the repository, and a
  disc console's whole way of starting one - an encrypted partition, the
  disc's own loader, an executable - has to be tested anyway. A synthetic
  disc around keys made up for the test, with a forty-line loader of our
  own and one of our test programs as the game, goes through every step,
  in every container format, interpreted and recompiled, as a CTest test.
- **Keys are the user's.** A console whose discs are encrypted needs a key
  that is not ours to ship. Ask for it once, accept it only by its
  decrypting something whose hash checks (so a wrong key is "wrong key"
  and never garbage), and let the recompiler embed the one disc's own key
  so that nothing installed or exported needs the first key again.
- **When games talk to an operating system, emulate the conversation.**
  A Wii game never touches the drive, the flash or the radio: it sends
  request blocks to a second processor. The registers and the block format
  are the whole interface, and behind them each device is a table of
  functions that can be written and tested alone. Write the kernel and the
  device interface first, commit them, and the devices parallelise: the
  disc reader, the filesystem and the Bluetooth stack were written at the
  same time against that one header.
- **Hardware-validated tests beat an oracle's.** The GameCube vectors were
  measured on an emulator and all passed. A test validated on the real
  processor found a bug in the first minute - a string load that zeroed
  one register too many, the stack pointer among them - and 268 results
  that differ in floating-point status. Look for a test someone ran on
  the hardware before trusting one measured on software.
- **Say what was not tested.** Everything above is against programs and a
  disc made here. Until a retail game has been through the gates the
  console stays unsupported, with a switch for whoever is testing, and
  the documents say so in their first paragraph.

## Choosing the next console

A console fits this process well when:

1. Per-instruction vectors exist for its CPU.
2. It has hardware test ROMs with a permissive license and a verdict that a
   program can read.
3. A reference emulator exists that can be run as a separate oracle.
4. Most of its code runs from ROM. Heavy self-modifying code, several CPUs
   sharing a bus, or coprocessors in the cartridge each add a phase.
5. Its BIOS, if any, can be emulated at high level.
6. The user owns a game to validate with.

Early leads, not yet checked against these criteria or for licenses:

- **Master System / Game Gear** is closest to the GB and NES paths: one
  8-bit CPU (a Z80), code in banked ROM, and a mapper per cartridge
  family. The NES backend's bank-keyed recompiler is the model.
- **Mega Drive** adds a second CPU (a Z80), as the SNES did with the
  SPC700. SNES, NES and GameCube are done.
- **Nintendo DS** reuses the ARM7TDMI core and adds an ARM9, 3D and
  firmware.

Doing P0 for a candidate settles its place in this list.

## Plan template

Copy this outline into `docs/<F>-PLAN.md` and fill it in during P0.
[GBA-PLAN.md](GBA-PLAN.md) is a complete example.

```markdown
# <Console> backend: plan

Started <date> on branch `feature/<f>-backend`.

## Ground rules
License policy, one semantics source, explicit guest state, interpreter from
day one, labelled evidence layers, fail closed.

## What we reuse
| Source | License | What we take |

## Layout
Directory tree of runtime-<f>/, recompiler-<f>/, tests/<f>/, tools/<f>/.

## Phases
P0-P8 from docs/ADDING-A-CONSOLE.md, each with the concrete suites and
numbers that make up its gate for this console.

## Checklist coverage
| Nuance | Test |

## Status
| Phase | State (date, numbers, what is left) |

## Behaviour pinned by the vectors
Non-obvious CPU behaviour that any rewrite (emitter included) must keep.

## Known deviations
Accepted test deviations with the case and the reason.
```
