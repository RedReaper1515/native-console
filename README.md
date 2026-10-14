# Native Console

Native Console turns console ROMs into native desktop executables and gives
you a hub to collect, build, and play them.

Drop a ROM or a folder of ROMs into the hub. Each game is statically
recompiled to C, linked against a shared SDL2 runtime, compiled with your host
toolchain, and added to a library with cover art, descriptions, and favorites.
Click a game to play the native build.

How far each console has come is not the same, and the hub shows it beside
each console:

| Console | Status |
|---|---|
| Game Boy, Game Boy Color, Game Boy Advance | **Playable.** The most tested, and the ones to start with ([accuracy](docs/ACCURACY.md), [GBC](docs/GBC.md), [GBA](docs/GBA.md)). |
| Super Nintendo, NES | **Some titles work.** One commercial game each has been checked through: Kirby's Dream Land 3, an SA-1 cartridge ([SNES](docs/SNES.md)), and Final Fantasy, an MMC1 cartridge ([NES](docs/NES.md)). Others may not start, or may break. |
| Nintendo DS, Nintendo 64, GameCube, Wii | **Not playable yet.** Games build and may start, but expect wrong graphics, missing sound and crashes ([DS](docs/NDS.md), [N64](docs/N64.md), [GameCube](docs/GC-PLAN.md), [Wii](docs/WII.md)). |

Releases are for Windows. Game Boy, Game Boy Advance and Super Nintendo
games can also be built from source for Linux / SteamOS and macOS, which
are less tested ([Linux](docs/LINUX.md), [macOS](docs/MACOS.md)).

The project contains no games: bring your own, dumped from cartridges and
discs you own. See [ROMs, piracy and preservation](#roms-piracy-and-preservation).

## ROMs, piracy and preservation

Native Console is a preservation project. Its purpose is to keep the games
you own playable on the computers you have now, and open to study and
modding, after the consoles they were made for have stopped working.

We do not encourage or condone piracy.

- **Nothing is included.** This repository holds no ROMs, disc images, BIOS
  or firmware files, encryption keys, or game graphics, sound or text, and
  none will be accepted into it. The tests run on test programs that are
  built here or openly licensed.
- **Use your own games.** Native Console is meant for ROMs and disc images
  you made yourself from cartridges and discs you own. It does not download
  games and does not tell you where to find them. Please do not ask.
- **Keep what you build.** A built or exported game contains the game's own
  code and data, so it is a copy of the game. It is for you, on your own
  machines; do not share or distribute it. The same goes for anything taken
  out of a game with the data and lifting tools.
- **Know your own law.** What you may do with a copy of a game you own
  differs from country to country. Following the law where you live is your
  responsibility.

Issues and pull requests that ask for ROMs, link to them, or contain
copyrighted game data will be closed.

## The hub

`native_console_hub` is the desktop front end (SDL2 + Dear ImGui).

- **Library**: cover-art grid or sortable list, search, per-console filter.
  Click a game for its detail panel; double-click or press Play to run it.
- **Import**: a single ROM, a whole folder (recursive), or drag and drop.
  Every supported ROM is copied into its own game folder and built on a
  background worker. The Jobs view streams the recompiler and compiler logs.
- **Metadata**: editable title, description, per-game launch arguments,
  favorites, play count, last played.
- **Cover art**: import PNG/JPEG/BMP/GIF, or capture a frame from the game.
  After each successful build the hub captures a title-screen frame so no
  game is blank.
- **Consoles**: one backend per hardware family, each with its own
  recompiler: `gbrecomp` (Game Boy, Game Boy Color), `gbarecomp`,
  `snesrecomp`, `nesrecomp`, `ndsrecomp`, `n64recomp` and `gcrecomp`
  (GameCube). How far each has been tested is at the top of this page.
  [Adding a console](docs/ADDING-A-CONSOLE.md) describes how a new family
  gets in.
- **Playing together**: families whose runtimes have a link open two windows
  on one computer, or host and join across a network — the Game Boy and GBA
  link cables.
- **Compile for export**: a Windows zip, a Linux / SteamOS tarball, or a
  macOS build kit, for the consoles that have been tested on each:

  | Console | Windows | Linux / SteamOS | macOS |
  | --- | --- | --- | --- |
  | Game Boy, Game Boy Color | yes | yes | yes |
  | Game Boy Advance | yes | yes | yes |
  | Super Nintendo | yes | yes | yes |
  | Nintendo DS, Nintendo 64, GameCube | yes | no | no |
  | NES, Wii | no | no | no |

- **Settings**: auto-detected toolchain paths (recompiler, CMake, Ninja,
  MSVC environment, SDL2), build type, compile jobs, launch defaults, and
  cover-capture frame.
- **Automation**: the same pipeline runs headless.

```bash
native_console_hub --data-dir <dir>
native_console_hub --data-dir <dir> --batch --import path/to/roms
native_console_hub --data-dir <dir> --batch --rebuild-all
```

Batch mode exits 0 only when every job succeeded. Each game folder keeps a
re-runnable build script and its own battery save next to the executable.
Details, the data layout, and how to add a console are in the
[hub guide](hub/README.md).

## Get a release

The [releases page](https://github.com/RedReaper1515/native-console/releases)
has the hub and the recompilers built for Windows (x64).

Every game is compiled on your own computer, so you still need a compiler:
install Visual Studio Build Tools (or Visual Studio) with the *Desktop
development with C++* workload. The hub finds it, and the CMake and Ninja
that come with it, by itself; SDL2 is in the archive. Then unzip, run
`native_console_hub.exe`, and drop a ROM on the window.

Linux, SteamOS and macOS are built from source for now.

## Build from source

Install CMake 3.20 or newer, Ninja, SDL2 development files, and a
C11/C++20 compiler (MSVC Build Tools on Windows).

```bash
git clone https://github.com/RedReaper1515/native-console.git
cd native-console
cmake -G Ninja -B build . -DSDL2_DIR=<path-to-sdl2-cmake-dir>
ninja -C build gbrecomp gbarecomp snesrecomp nesrecomp ndsrecomp n64recomp gcrecomp native_console_hub
./build/bin/native_console_hub --data-dir ~/native-console-hub
```

The hub finds the recompilers (`gbrecomp`, `gbarecomp`, `snesrecomp`,
`nesrecomp`, `ndsrecomp`, `n64recomp`, `gcrecomp`) next to itself and
detects the rest of the toolchain; anything it cannot find is editable in
Settings. A console whose recompiler was not built cannot be built by the
hub.

## Under the hood

```text
ROM -> decoder and analyzer -> IR + metadata -> generated C project
                                              + versioned runtime snapshot
                                              -> host compiler -> executable
        \_______________________ hub drives this per game _______________/
```

- `recompiler/` decodes and analyzes banked LR35902 code, emits reviewable C
  and machine-readable metadata, and falls back safely to an interpreter for
  anything static analysis did not compile.
- `runtime/` provides DMG/CGB execution, cartridge mappers, SDL video, audio,
  input, battery saves, RTC data, savestates, and the ImGui launcher.
- `hub/` catalogues games, generates and runs each game's build script, and
  launches the results. Console backends live in `hub/src/console_*.cpp`.
- `gbrecomp` remains usable on its own for scripted generation:

```bash
./build/bin/gbrecomp path/to/game.gb -o output/game --compact-resume
cmake -G Ninja -S output/game -B output/game/build
ninja -C output/game/build
./output/game/build/game
```

`--compact-resume` is what the hub uses: smaller executables with identical
guest behavior. Leave it off for builds you intend to single-step with
`--differential`.

Three analyzer options decide how much of the cartridge is compiled. Anything
left out still runs exactly through the interpreter fallback, so they trade
size for a little speed, never correctness. ROM byte ranges that look like
ASCII text or graphics are classified as data before analysis (disable with
`--no-classify-data`), `--pointer-scan strict|off` limits or disables the
entry points seeded from 16-bit values found in the ROM, and where two
instruction streams decode the same bytes from different alignments only the
alignment that explains more of the ROM is kept (disable with
`--no-resolve-overlaps`; each exclusion is reported as an
`overlapping_decode_excluded` diagnostic). On a 512 KB commercial ROM the
`loose` scan compiles 284K guest instructions, the classifier alone brings
that to 208K, `strict` to 154K, and overlap resolution to 134K, with the
interpreter covering well under 1% of executed instructions.

This is an experimental recompiler, not a whole-catalogue compatibility claim.
Hardware accuracy and game coverage remain active work, and a successful
build does not guarantee that every scene runs correctly.

## Game Boy Advance

`recompiler-gba/` (`gbarecomp`) and `runtime-gba/` recompile `.gba` ROMs the
same way; the hub builds them like Game Boy games. [GBA.md](docs/GBA.md) describes
what works, and [docs/GBA-PLAN.md](docs/GBA-PLAN.md) holds the plan, the test
strategy and the current results.

## Super Nintendo (some titles work)

`recompiler-snes/` (`snesrecomp`) and `runtime-snes/` recompile `.sfc` and
`.smc` ROMs, SA-1 cartridges included, in the same way. [SNES.md](docs/SNES.md)
describes what works, and [docs/SNES-PLAN.md](docs/SNES-PLAN.md) holds the
plan, the test strategy, how each item of the SNES assembly-to-C nuances list
is tested, and the current results.

## NES (some titles work)

`recompiler-nes/` (`nesrecomp`) and `runtime-nes/` recompile `.nes` ROMs of
the common cartridge boards. Cartridges switch banks at run time, so
recompiled code is keyed by the bank mapped at its address.
[NES.md](docs/NES.md) describes what works, and
[docs/NES-PLAN.md](docs/NES-PLAN.md) holds the plan, the test strategy and
the current results.

## Nintendo DS (not playable yet)

`recompiler-nds/` (`ndsrecomp`) and `runtime-nds/` recompile `.nds` ROMs:
both processors from the game's own code, with the screens, the 3D engine,
sound, the touch screen, the microphone and the save chips in the runtime,
and no BIOS or firmware file. A game's overlays load over one another, so a
native function runs only while memory holds the bytes it was compiled
from. [NDS.md](docs/NDS.md) describes what works, and
[docs/NDS-PLAN.md](docs/NDS-PLAN.md) holds the plan, the test strategy and
the current results.

## Nintendo 64 (not playable yet)

`recompiler-n64/` (`n64recomp`) and `runtime-n64/` recompile `.z64`, `.n64`
and `.v64` ROMs: the VR4300's code to C, with the RSP, the RDP (a software
port of parallel-rdp), sound, controllers and cartridge saves in the
runtime, and no boot ROM. krom's CPU, FPU and RSP test ROMs pass
interpreted and recompiled, and the first game, Ocarina of Time, builds
and starts from the hub on Windows, but is not playable yet. [N64.md](docs/N64.md) describes what works, and
[docs/N64-PLAN.md](docs/N64-PLAN.md) holds the plan, the test strategy and
the current results.

## GameCube (not playable yet)

`recompiler-gc/` (`gcrecomp`) and `runtime-gc/` recompile GameCube discs: the
processor's code to C, with the graphics processor, sound, the disc, memory
cards and controllers in the runtime. One game has been tested, Shrek 2. It
builds and runs from the hub on Windows, but it is not playable yet, and
other games will need more. [docs/GC-PLAN.md](docs/GC-PLAN.md) holds
the plan and a dated log of the results.

## Wii (not playable yet)

The Wii is the GameCube's design with more of everything, and is built here
as the GameCube family's second console: the same `gcrecomp` and
`runtime-gc/`, plus MEM2, Hollywood's registers, IOS emulated at the level a
game sees it (the disc, the flash filesystem, the remotes) and encrypted
discs. It passes its tests against programs and a disc built in this
repository; no retail game has been verified yet, so the hub keeps it
switched off unless asked. A retail disc needs your console's common key
once, to build; none ships here. [WII.md](docs/WII.md) describes what works and
how to try it, and [docs/WII-PLAN.md](docs/WII-PLAN.md) holds the plan and
the results.

## Adding a console

Each console family is a recompiler (`recompiler-<f>/`), a runtime
(`runtime-<f>/`) and a hub backend (`hub/src/console_<f>.cpp`). A family
that meets the shared contract gets library builds, Linux / SteamOS and
macOS builds, and export with no changes to that code: a self-contained
CMake project, the ROM embedded, SDL2 as the only dependency, and
deterministic headless runs. The hub turns each export target on for a
console once one of its games has been exported there and run
(`export_problem` in its backend). Every console's games also get an in-game
overlay menu with the same controls.
[docs/ADDING-A-CONSOLE.md](docs/ADDING-A-CONSOLE.md) is the playbook. It
lists the contract, the phases from test data to first commercial game with
the gate for each phase, every file to touch, and the lessons from the GBA
backend.

## Documentation

- [Hub](hub/README.md) — library, import/build pipeline, launcher, automation
- [Game Boy Advance](docs/GBA.md) — status, tools and usage; [plan](docs/GBA-PLAN.md)
- [Super Nintendo](docs/SNES.md) — status, tools and usage; [plan](docs/SNES-PLAN.md)
- [NES](docs/NES.md) — status, tools and usage; [plan](docs/NES-PLAN.md)
- [Nintendo DS](docs/NDS.md) — status, tools and usage; [plan](docs/NDS-PLAN.md)
- [Nintendo 64](docs/N64.md) — status, tools and usage; [plan](docs/N64-PLAN.md)
- [GameCube](docs/GC-PLAN.md) — the plan and a dated log of results
- [Wii](docs/WII.md) — status, the key a disc needs, usage; [plan](docs/WII-PLAN.md)
- [Adding a console](docs/ADDING-A-CONSOLE.md) — contract, phases and gates, file checklist
- [Runtime usage](docs/RUNTIME.md) — controls, persistence, diagnostics, profiling
- [Accuracy](docs/ACCURACY.md) — external-test evidence and limitations
- [Game Boy Color status](docs/GBC.md) — implemented CGB behavior and gaps
- [Native replacement SDK](docs/NATIVE_PATCHES.md) — exact-ROM hooks
- [Port modules](docs/PORT_MODULES.md) — semantic access and host presentation
- [Data-mod packages](docs/DATA_MODS.md) — deterministic ROM-free overlays
- [The ROM's data as files](docs/DATA_FILES.md) — a Game Boy ROM taken apart into files, edited, and put together again for the game to load
- [Recorded runs](docs/RUN_RECORDS.md) — a played run kept with the cycle of every button, replayed exactly, and used to record what the game does over hours of real play
- [Discovery tool](docs/DISCOVERY.md) — find and name variables and sprite graphics by experiment
- [Texture packs](docs/TEXTURE_PACKS.md) — high-resolution tile replacement at runtime
- [Android](docs/ANDROID.md) — single-ROM Android generation, inherited from gb-recompiled and untested
- [Linux and SteamOS](docs/LINUX.md) — native Linux builds from Windows through Docker (from source, less tested)
- [macOS](docs/MACOS.md) — universal `.app` bundles (from source, less tested)
- [Project backlog](TODO.md) — prioritized remaining work

Contributors and coding agents should read [AGENTS.md](AGENTS.md) before
changing analysis, code generation, or hardware behavior.

## License

Native Console is available under the [MIT License](LICENSE), which carries
the copyright of the original gb-recompiled author and of Native Console's.
Native Console began as a
fork of [arcanite24/gb-recompiled](https://github.com/arcanite24/gb-recompiled).
Dear ImGui remains under its upstream license in the vendored runtime
snapshot.

Game Boy, Game Boy Advance, Super Nintendo, Nintendo Entertainment System,
Nintendo DS, Nintendo 64, GameCube and Wii are trademarks of Nintendo, and
the games named in this repository are trademarks of their owners. They are
named only to say what has been tested.
This project is not affiliated with or endorsed by Nintendo or any game's
publisher. Code ported from
ares (ISC) and other projects is listed in [THIRD_PARTY.md](THIRD_PARTY.md).
