# Super Nintendo (in progress)

The hub recompiles and plays `.sfc` and `.smc` ROMs, including SA-1
cartridges. Every test ROM we have gives the same result recompiled as
interpreted, and runs clean in cycle-exact lockstep against the interpreter
(88/88). Kirby's Dream Land 3 (USA), an SA-1 game, builds through the hub
and plays through the intro, title, file select, the level map and into the
first stage. On 6,500 frames of scripted play both of its CPUs run 100.0 %
native code, clean in `--differential`, and the Windows (MSVC) and Linux
(gcc 14, Steam Runtime) builds end in byte-identical state. The plan, phase
gates, how each item of the SNES assembly-to-C checklist is tested, and the
current results are in [docs/SNES-PLAN.md](docs/SNES-PLAN.md).

## Layout

```text
runtime-snes/     65C816 core (interpreter + shared instruction handlers, used
                  by the S-CPU and the SA-1), machine (memory map, I/O, DMA/HDMA,
                  timing, interrupts), PPU, SPC700 + S-DSP, SA-1, save states,
                  recompiled-code dispatcher, headless runner, SDL2 frontend
                  and overlay menu (platform/)
recompiler-snes/  snesrecomp: analyzer and C emitter
tests/snes/       SingleStepTests harnesses (65C816, SPC700), PPU, APU and SA-1
                  unit tests, test-ROM baseline, menu self-test
tools/snes/       test-ROM runner, recompile suite, 65C816 assembler and
                  disassembler, the checklist ROM, frame comparison
```

## Using it

From the hub: import a `.sfc` or `.smc` ROM. The installed folder holds
`<id>.exe`, `SDL2.dll`, the battery save `<id>.sav` and save states
`<id>.state1` to `.state10`; all of them survive a rebuild. The ROM is
compiled into the executable and no firmware file is needed (the APU's boot
ROM is our own). Hub builds profile the game first (about 20 s) and keep the
trace in the game's `profile/` folder; any other `*.txt` trace put there is
used by the next rebuild too.

By hand:

```bash
./build/bin/snesrecomp game.sfc --profile output/game.profile/boot.txt -o output/game
cmake -G Ninja -S output/game -B output/game/build -DCMAKE_BUILD_TYPE=Release -DSDL2_DIR=<SDL2 cmake dir>
ninja -C output/game/build
./output/game/build/game                                   # window, sound, input
./output/game/build/game --frames 600 --dump-frame last.ppm
./output/game/build/game --differential --frames 6000 --keys 300:START
./output/game/build/game --coverage --frames 6000 --auto-input
```

A trace from longer play covers more code: run the game with
`--trace-out FILE` (and `--keys` or `--auto-input`), then pass
`--trace FILE` to `snesrecomp`. Generated code calls the opcode handlers out
of line (as the interpreter does): Kirby's Dream Land 3 builds in 6.6 s
instead of 55 s and runs as fast. `--inline-handlers` inlines them into every
instruction again. Without a project, `snes_play game.sfc` plays
any ROM with the interpreter, and `snes_run game.sfc` runs it headless.

Headless options (the runner; any of them selects it over the window):
`--frames N`, `--until-idle`, `--expect-text`, `--fail-text`,
`--expect-wram ADDR=VALUE`, `--screen-text`, `--dump-frame FILE.ppm`,
`--dump-state FILE`, `--dump-audio FILE.wav`, `--keys FRAME:BUTTONS[:N]`,
`--auto-input`, `--differential`, `--coverage`, `--trace-out FILE`,
`--trace-last N`, `--trace-apu`, `--apu-console`, `--mode7-scale N`,
`--dump-large FILE.ppm`. The runner never writes battery saves.

Window options: `--scale N`, `--fullscreen` and `--mute` (each this run only),
`--save-dir DIR`, `--limit-frames N`, `--screenshot-prefix P`,
`--audio-stats`, `--prefs FILE`, `--load-state SLOT` (load save-state slot
1-10 at start), `--display-shot FILE.bmp` with `--display-size WxH` (on
exit, write the last frame as the display shows it at that size, default
3840x2160, through the saved scaling mode and filter), `--mode7-scale N`
(this run only). `--interpreter` works in both.

**Mode 7 resolution.** The mode 7 plane (the rotating, scaling and tilted
backgrounds) can be drawn with two, three or four samples a pixel each way:
Mode 7 Resolution in the settings menu, or `--mode7-scale N` for one run.
Edges of a turned or tilted plane lose their staircase, and a plane seen
from far away keeps detail the console's 256 pixels drop. Samples are taken
between lines as well as along them; where a game gives each line its own
matrix (a floor in perspective) the rows in between carry the matrix on by
its change since the line above. The console's own picture is still drawn,
exactly as before, and is what the game, save states, screenshots, the
hub's covers and the CRT filter use; a frame that shows the plane is
displayed from the larger picture. Sprites, the backdrop and lines of
other modes are the console's pixels, repeated. A texel that already
covers several pixels stays a square: nothing is filtered. Not drawn
larger: lines with mosaic on the plane, interlaced frames and pseudo-hires
lines. On a full-screen plane it costs about 1 ms a frame at 2x and 2.8 ms at 4x
(`--dump-large FILE.ppm` writes the larger picture headless).

Scale filters: **Nearest**, **Linear**, and **CRT**
(`runtime-snes/src/snes_crt.c`, a copy of the GBA runtime's), which draws
the picture as a CRT television shows it: each line is a glowing beam,
blurred a little along the line, that is thin where the picture is dark and
swells as it brightens; an aperture grille of red, green and blue stripes is
multiplied over it once a pixel is 3 screen pixels wide, and a soft halo is
added around bright areas. It runs on the CPU at whole screen rows per line
(6 at 1440p fullscreen), about 4.5 ms per frame there; 512-wide frames get 2
columns per pixel instead of 4. Below 2 screen rows per line (an interlaced
frame's 448 lines count) it draws Linear.

Controls (rebindable in the menu): arrows, X = A, Z = B, S = X, A = Y,
Q = L, W = R, Enter = Start, Backspace or Right Shift = Select, Tab = fast
forward (hold). Escape or F10 opens the menu (the game pauses); on a
controller Guide or L3 do. F9 shows an FPS counter in the window's corner (frames per second and the
game's speed, 100% being full speed; also **Show FPS Counter** in the menu). Controllers use the Nintendo layout. Preferences
are kept in `snes_prefs.ini` in the per-user app data folder, shared by all
SNES games.

## Tests

```bash
python tools/fetch_snes_tests.py            # pinned suites into roms/snes/ (local only)
ctest --test-dir build -R snes              # vectors, unit tests, ROM baseline, recompiled suites
python tools/snes/run_snes_tests.py --baseline tests/snes/rom_baseline.txt
python tools/snes/recomp_suite.py           # every test ROM recompiled: verdict parity and --differential
python tools/snes/run_checklist.py          # the assembly-to-C checklist ROM, interpreted
```

The checklist ROM (`tools/snes/checklist`) is assembled from source by our
own assembler, so it needs no fetched data: 43 tests covering the SNES
assembly-to-C nuances list, each passing interpreted and recompiled.
`docs/SNES-PLAN.md` maps all 100 items of that list to the test that covers
them.

`snes_ppu_units` checks the larger mode 7 picture against the console's
formula in exact arithmetic, sample by sample, for turned, enlarged, reduced
and flipped planes at 2x, 3x and 4x, with a window, with a matrix that
changes every line, and next to lines of another mode; and that the
console's rows do not change with the scale.

## What is not done

- Coprocessors other than the SA-1 (Super FX, DSP-1 to 4, Cx4, S-DD1,
  SPC7110, OBC1, S-RTC): such ROMs are refused at import with the chip's name.
- SA-1 bus conflicts (the SA-1 waiting while the S-CPU uses the same memory)
  are not modelled. If a game switches Super MMC banks, its code runs
  interpreted until the banking is back to the power-on layout.
- PAL timing (312 lines, 50 Hz) and PAL region detection.
- Timing details listed for P8 in the plan: dot-exact H/V IRQ positions, the
  interrupt-delay (irqLock) after DMA and NMITIMEN writes, mid-scanline PPU
  register effects.
- The hub captures covers at the frame set in Settings (600 by default), which
  for some games is still a mostly blank intro screen.
