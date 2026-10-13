# Linux and SteamOS

Game Boy, Game Boy Color, Game Boy Advance and Super Nintendo games build as native x86-64
Linux executables. They run on SteamOS (Steam Deck) and on any desktop Linux
with glibc 2.29 or newer and SDL2. The ROM is compiled into the executable, the
same as on Windows.

## Build from Windows

`tools/steamos/build_steamos.ps1` builds inside Valve's Steam Runtime 3
("sniper") SDK container, the environment Steam uses for native Linux games.
You need Docker Desktop running; the first run downloads the SDK image
(about 5.4 GB unpacked).

```powershell
tools\steamos\build_steamos.ps1 -Rom "D:\roms\Link's Awakening.gb" -Name links_awakening
tools\steamos\build_steamos.ps1 -Rom "D:\roms\Minish Cap.gba" -Name minish_cap -Trace trace1.txt,trace2.txt
```

| Parameter | Purpose |
| --- | --- |
| `-Rom` | ROM file; `.gba` selects the GBA recompiler, `.sfc`/`.smc` the SNES one, anything else the Game Boy one (`tools/consoles.sh`) |
| `-Name` | Game id: executable name and output folder (letters, digits, underscores) |
| `-Console gb\|gba\|snes` | Override the choice made from the extension |
| `-Trace` | `--trace` files for `gbarecomp` or `snesrecomp` (a hub game's `profile/boot.txt`, or `snesrecomp ROM --profile FILE`) |
| `-RecompilerArgs` | Extra recompiler arguments, such as `--compact-resume` |
| `-OutDir` | Destination; defaults to `output/steamos` in the checkout |

The output is `<id>/<id>` plus `<id>-steamos-x86_64.tar.gz`, the same folder
with the executable bit kept. Copying the bare file from Windows loses that bit,
so copy the archive. Builds are cached in the Docker volume
`native-console-steamos`, so later builds reuse the compiled recompilers.
Remove the volume with `docker volume rm native-console-steamos`.

## Install on a Steam Deck

1. In Desktop Mode, copy `<id>-steamos-x86_64.tar.gz` to the Deck and extract
   it, for example into `~/Games`.
2. In Steam, choose **Add a Non-Steam Game**, browse to `~/Games/<id>/<id>` and
   add it.
3. Launch it from Game Mode. Battery saves (`<id>.sav`) are written next to the
   executable.

SteamOS includes SDL2 (gamescope depends on it), so the game runs on the host
as it is. To run it in the exact environment it was built for, set the
shortcut's **Properties → Compatibility** tool to *Steam Linux Runtime 3.0
(sniper)*.

## Build on Linux directly

Any distribution with CMake 3.20+, Ninja, a C++20 compiler and the SDL2
development package works:

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
ninja -C build
./build/bin/gbrecomp game.gb -o output/game --output-prefix game
cmake -G Ninja -S output/game -B output/game/build -DCMAKE_BUILD_TYPE=Release
ninja -C output/game/build
```

An executable built on a newer distribution needs that distribution's glibc
or newer. Use the SDK container above for anything you share.

## Compatibility

The Linux and Windows builds of a game are the same program. Given the same
scripted input, Link's Awakening (2,400 frames) produced a byte-identical
`--dump-state` and identical frames on Windows (MSVC) and Linux (gcc 14). The
same held for The Minish Cap (3,000 frames). The Linux executables were run
in the sniper SDK, the sniper platform runtime and a plain Arch Linux system
with `sdl2-compat`. All 61 CTest tests pass on Linux except two whose test
scripts need Python 3.10 (the SDK has 3.9):
`nl0_profile_summary_estimates_dynamic_coverage` and
`native_patch_sdk_end_to_end`.

The hub's **Compile for export** makes the same Linux build from Windows,
also through Docker and the sniper SDK. It writes `<id>-linux-x86_64.tar.gz`
for each ROM you choose; see [hub/README.md](hub/README.md#compile-for-export).
