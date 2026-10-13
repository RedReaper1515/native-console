# NES (in progress)

The hub recompiles and plays `.nes` ROMs of the common cartridge boards.
Of the 160 hardware test ROMs we run, 158 pass in the interpreter, and all
of them give the same result recompiled and run clean in cycle-exact
lockstep against the interpreter. Final Fantasy (USA), an MMC1 game, builds
through the hub in about 12 seconds and plays through the intro, party
creation, the world map and into battle. On 9,000 frames of scripted play it
runs 99.9 % native code, clean in `--differential`, and a save state made
by recompiled code resumes exactly in the interpreter and the other way
round. The plan, phase gates and current results are in
[docs/NES-PLAN.md](docs/NES-PLAN.md).

## Layout

```text
runtime-nes/     6502 core (interpreter + shared instruction handlers),
                 machine (memory map, controllers, OAM DMA, device clocks,
                 interrupts), cartridge boards, dot-accurate PPU, APU, save
                 states, recompiled-code dispatcher, headless runner, SDL2
                 frontend and overlay menu (platform/)
recompiler-nes/  nesrecomp: analyzer and C emitter
tests/nes/       SingleStepTests harness, PPU, APU and cartridge unit tests,
                 test-ROM baseline, menu self-test
tools/nes/       test-ROM runner, recompile suite, 6502 assembler, the
                 checklist ROM, nestest trace comparison
```

## Cartridges

By iNES mapper number: NROM (0), MMC1 (1), UxROM (2), CNROM (3), MMC3 (4),
AxROM (7), MMC2 (9), MMC4 (10), Color Dreams (11), BNROM / NINA-001 (34),
GxROM (66), Camerica (71). These cover roughly nine in ten licensed games.
A ROM that needs another board is refused at import with its mapper number.

Only NROM and MMC1 have run commercial-size programs so far (the test ROMs
and Final Fantasy); MMC3 passes blargg's mapper tests for the common chip
revision, and the others are covered by unit tests on synthetic images.

## Using it

From the hub: import a `.nes` ROM. The installed folder holds `<id>.exe`,
`SDL2.dll`, the battery save `<id>.sav` and save states `<id>.state1` to
`.state10`; all of them survive a rebuild. The ROM is compiled into the
executable; the NES has no firmware. Hub builds profile the game first
(about 6 s) and keep the trace in the game's `profile/` folder; any other
`*.txt` trace put there is used by the next rebuild too.

By hand:

```bash
./build/bin/nesrecomp game.nes --profile output/game.profile/boot.txt -o output/game
cmake -G Ninja -S output/game -B output/game/build -DCMAKE_BUILD_TYPE=Release -DSDL2_DIR=<SDL2 cmake dir>
ninja -C output/game/build
./output/game/build/game                                   # window, sound, input
./output/game/build/game --frames 600 --dump-frame last.ppm
./output/game/build/game --differential --frames 6000 --keys 300:START
./output/game/build/game --coverage --frames 6000 --auto-input
```

### How code is found

A cartridge switches banks, so one address holds different code at
different times. Recompiled code is therefore keyed by the address and the
8 KiB PRG ROM page mapped there, and the run loop looks a key up from the
cartridge's real page table. A compiled entry can never run against the
wrong bytes; what the recompiler guesses about banking only decides how much
gets compiled. Code it missed runs in the interpreter.

`nesrecomp` starts from the reset vector and the profile, follows calls and
jumps (a target in another bank is tried in every bank it could be in, and
kept only if it decodes as plausible code), and reads the jump tables it
finds next to `JMP (ptr)` and push/`RTS` dispatch code. A trace from longer
play covers more: run the game with `--trace-out FILE` (and `--keys` or
`--auto-input`), then pass `--trace FILE` to `nesrecomp`. `--coverage` shows
what still ran interpreted.

Without a project, `nes_play game.nes` plays any ROM with the interpreter,
and `nes_run game.nes` runs it headless.

Headless options (the runner; any of them selects it over the window):
`--frames N`, `--until-idle`, `--blargg` (blargg's `$6000` result protocol),
`--expect-ram ADDR=VALUE`, `--expect-text` (repeatable), `--expect-text-any`,
`--fail-text`, `--screen-text`, `--smoke` (still running, and a picture with
at least three colours), `--pc ADDR`, `--dump-frame FILE.ppm`,
`--dump-state FILE`, `--dump-audio FILE.wav`, `--keys FRAME:BUTTONS[:N]`,
`--auto-input`, `--differential`, `--coverage`, `--trace-out FILE`,
`--cpu-log FILE`, `--trace-last N`, `--save-state FILE` with
`--save-state-frame N`, `--load-state FILE`. The runner never writes battery
saves.

Window options: `--scale N`, `--fullscreen` and `--mute` (each this run only),
`--save-dir DIR`, `--limit-frames N`, `--screenshot-prefix P`,
`--audio-stats`, `--prefs FILE`, `--load-state SLOT`, `--display-shot
FILE.bmp` with `--display-size WxH`, `--menu-shot FILE.bmp` (on exit, write
the window as it looks with the settings menu open). `--interpreter` works
in both.

The menu's **Overscan** setting (on by default) hides the top and bottom 8
lines, as a television does; many games leave garbage there. Scale filters:
**Nearest**, **Linear** and **CRT** (`runtime-nes/src/nes_crt.c`, the SNES
runtime's filter).

Controls (rebindable in the menu): arrows, X or L = A, Z or K = B, Enter =
Start, Backspace or Right Shift = Select, Tab = fast forward (hold). Escape
or F10 opens the menu (the game pauses); on a controller Guide or L3 do.
F9 shows an FPS counter in the window's corner (frames per second and the
game's speed, 100% being full speed; also **Show FPS Counter** in the menu). On
a controller, A is the bottom and right face buttons and B the left and top
ones. A second controller plays as player 2. Pressing left and right (or up
and down) together passes neither on, as no real pad can. Preferences are
kept in `nes_prefs.ini` in the per-user app data folder, shared by all NES
games.

## Tests

```bash
python tools/fetch_nes_tests.py             # pinned suites into roms/nes/ (local only)
ctest --test-dir build -R "^nes_"           # vectors, unit tests, checklist ROM, ROM baseline
python tools/nes/run_nes_tests.py --baseline tests/nes/rom_baseline.txt   # 161 ROMs, 6 s
python tools/nes/recomp_suite.py --build build   # every test ROM recompiled: verdict parity and --differential (80 s)
python tools/nes/accuracycoin.py --runner build/bin/nes_run               # AccuracyCoin's 146 tests, one line each
python tools/nes/run_checklist.py --runner build/bin/nes_run
python tools/nes/state_resume.py --exe output/game/build/game --recompiled --out build/state_resume -- --auto-input
```

Each suite reports its own way, and the runner reads each the way it
reports: blargg's newer ROMs through `$6000`, the 2005 ones through a
zero-page result byte once the ROM has stopped, nestest by its trace,
AccuracyCoin by its result table. A missing signature, a ROM that never
finishes or one that is not in the catalogue is a failure.

The checklist ROM (`tools/nes/checklist`) is assembled from source by our
own assembler (`tools/nes/asm6502.py`), so it needs no fetched data: 54
tests of what a recompiler gets wrong on this machine (bank switching under
running code, instructions that straddle a bank boundary, computed jumps,
code in RAM, interrupt timing, I/O side effects), each passing interpreted
and recompiled.

## What is not done

- Other cartridge boards (MMC5, VRC, Namco, Sunsoft and the rest), the
  Famicom Disk System, Vs. System and PlayChoice images, expansion audio.
- PAL and Dendy timing (312 lines, 50 Hz): PAL images are refused at import.
- Code that runs from RAM is always interpreted.
- Zapper, four-player adapters and other controllers.
- The accuracy items listed under "Known deviations" in the plan: the other
  MMC3 revision, and 15 of AccuracyCoin's 146 tests (DMA corner cases and
  switching rendering on and off in the middle of a line).
- Linux / SteamOS and macOS builds have not been tried yet (P7).
- The hub captures covers at the frame set in Settings (600 by default), which
  for some games is still an intro screen.
