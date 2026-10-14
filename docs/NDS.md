# Nintendo DS (in progress)

> **Not playable yet.** Games build and may start, but expect wrong
> graphics, missing sound and crashes. The hub marks Nintendo DS games the
> same way.

The hub recompiles and plays `.nds` ROMs: both of the console's processors
(the ARM9 and the ARM7) are recompiled from the game's own code, and the
two screens, the 3D engine, sound, the touch screen, the microphone, the
lid, the save chips and the real-time clock are the runtime's. No BIOS or
firmware file is needed.

The Legend of Zelda: Phantom Hourglass (USA) imports, builds in about 70
seconds and plays from the hub. Built without any help from its
decompilation, it runs 100.00 % native code on both processors over a
14,400-frame scripted route (title, a new file, name entry, the prologue,
the first scene on the ship), ends that route in the same state bit for bit
as a build that uses the decompilation's symbols, and is clean in
`--differential` over 6,000 scripted frames. In its window it holds full
speed with no audio underruns. The plan, phase gates and current results
are in [docs/NDS-PLAN.md](NDS-PLAN.md).

One commercial game has been taken through so far. Other games should be
expected to find gaps; see [What is not done](#what-is-not-done).

## Layout

```text
runtime-nds/     ARM9 (ARMv5TE) and ARM7 (ARMv4T) cores from one set of
                 instruction handlers, the machine (both memory maps, I/O,
                 timers, DMA, the IPC FIFO, the maths unit, the card, the
                 two-CPU scheduler), 2D engines, 3D geometry and software
                 rasteriser, sound, SPI devices (firmware, touch, power),
                 RTC, save chips, replacement BIOS images, recompiled-code
                 dispatcher, headless runner, SDL2 frontend and overlay
                 menu (platform/)
recompiler-nds/  ndsrecomp: ROM and overlay reader, analyzer, C emitter
tests/nds/       instruction vectors, device unit tests, analyzer tests,
                 test-ROM baselines, the menu self-test
tools/nds/       test-ROM runners, the recompile suite, oracle scripts
                 (DeSmuME), the BIOS and test-ROM builders
```

## What a ROM needs

- A dump with a **decrypted secure area**, which is what dumping tools
  normally write. An encrypted one is refused at import: decrypting it
  takes a key table from the console's BIOS, which is never shipped here.
- DS and DSi-enhanced games (the latter run as DS games). DSi-only titles
  are refused.
- Nothing else. The BIOS is replaced by DraStic's open implementation
  (BSD-2-Clause, assembled by `tools/nds/build_freebios.py`), and the
  firmware's user settings (nickname, language, touch calibration) are made
  up by the runtime and editable in the menu.

The whole ROM is embedded in the executable, up to the 512 MiB a DS card
can hold. The save chip is chosen from a table of 6,776 games and, for a
game not in it, recognised from the first write the game makes.

## Using it

Import the `.nds` file in the hub. To do the same by hand:

```bash
ndsrecomp game.nds --profile profile/boot.txt -o out --name game
cmake -G Ninja -S out -B out/build -DCMAKE_BUILD_TYPE=Release
ninja -C out/build
out/build/game                       # the window
out/build/game --frames 600 --dump-frame title.ppm --quiet
```

The executable opens a window unless a test option asks for a headless run
(`--frames`, `--dump-frame`, `--differential`, `--coverage` and the rest of
[nds_runner.c](../runtime-nds/src/nds_runner.c)'s list). `nds_run` and
`nds_play` are the same two programs for a ROM file, with the interpreter
alone.

In the window:

| | Keyboard | Controller |
|---|---|---|
| D-pad | arrows or WASD | D-pad, left stick |
| A, B, X, Y | Z or J, X or K, C or I, V or U | east, south, north, west (Nintendo layout) |
| L, R | Q, E | shoulders |
| Start, Select | Enter, Backspace or Right Shift | Start, Back |
| Stylus | the mouse on the bottom screen; a finger on a touch screen | right stick moves a cursor, right trigger puts the pen down |
| Blow or shout into the microphone | Space (held) | left trigger (held) |
| Close the lid | F4 (held) | not bound; the menu has "Close the Lid for Two Seconds" |
| Next layout, swap screens | F2, F3 | not bound, R3 |
| Fast forward, mute | Tab (held), M | not bound |
| Fullscreen, screenshot | F11, F12 | not bound |
| FPS counter (frames per second and the game's speed) | F9 | not bound; the menu has "Show FPS Counter" |
| Settings menu | Escape or F10 | Guide, or L3 |

On a Mac the shortcuts use Command instead of the F-keys. The lid is closed
only while its key is held: the DS sleeps with its lid shut, so a lid left
closed would look like a frozen game, and what games ask for is to close it
and open it again.

Four screen layouts: stacked, side by side, one large and one small, and a
single screen, each with the screens swapped if wanted. The settings menu
rebinds everything for keyboard and controller, and sets the layout,
scaling (whole multiples, or fit the display) and filter, volume and speed,
the microphone (a held button, or this computer's microphone passed
through) and the console's nickname and language. It also shows how many
instructions the last frame had to interpret.

**Internal resolution.** The 3D engine's picture can be drawn at two,
three or four times the screens' 256 by 192: Internal Resolution in the
settings menu, or `--internal-scale N` for one run. Polygons get sharper
edges and finer texture detail; backgrounds, sprites and text are the
console's own pixels, made larger. The game itself still sees the
console's picture, so nothing it does can depend on the setting. It costs
processor time, shared between threads: on the development machine 3x adds
under 3 ms to a 16.7 ms frame and 4x about 6 ms. Games that put 3D on
both screens at once do it by showing one screen from a picture captured
the frame before; that screen is drawn larger too, as long as the game
shows the capture as it was taken (Phantom Hourglass's opening does).

Files: the card's save is `<name>.sav` next to the executable (`--save-dir`
puts it elsewhere); settings (`nds_prefs.ini`) and the console's own memory
(`nds_firmware.bin`) are in the per-user application data folder, shared by
all DS games. Scripted runs pass `--prefs FILE` to keep away from both.

### How code is found

A DS game is several programs: the ARM9 program with blocks it copies to
fast memory at start-up, dozens of overlays that load over one another at
the same addresses, and the ARM7 program. `ndsrecomp` reads them all out of
the ROM (decompressing where needed) and analyses each as an image at the
address it runs at.

- **No overlay tracking.** Every native function carries a hash of the
  bytes it was compiled from and runs only while memory holds those bytes
  there. Several functions can share an address, one per overlay; the one
  in memory runs. A wrong guess by the analyzer costs speed, never
  correctness.
- **Without symbols** the analyzer follows calls from the entry points,
  reads pointers out of data, and recognises small functions that are only
  ever pointed at. Against Phantom Hourglass's decompilation it finds 99.0 %
  of the 24,608 functions.
- **The profile** (`--profile`, which the hub always runs) plays the game
  for 6,000 frames with the interpreter under a built-in script of taps,
  strokes and button presses, and records what ran. That is how interrupt
  handlers and code a program moves before running it are found.
- **More traces.** A game's `--trace-out FILE` (headless) writes what it
  still had to interpret; put the file in the game's `profile/` folder in
  the hub's data and rebuild.
- **Symbols.** A [ds-decomp](https://github.com/AetiasHax/ds-decomp)
  configuration for the game (`--symbols DIR`; in the hub,
  `profile/symbols/config.yaml`) gives the analyzer the decompilation's
  function list. It is optional, and must match the ROM exactly.

Code that is not found is interpreted, with the same result.

## Tests

```bash
python tools/fetch_nds_tests.py      # test data into roms/nds/ (not committed)
ctest --test-dir build -R "^nds_"
```

- Instruction vectors: 2,250,000 ARM7TDMI vectors and 530,000 generated
  ARMv5TE vectors, all passing.
- Device unit tests for the machine, both 2D engines, 3D geometry and the
  rasteriser (including nds-interp's 198,404 measured scanlines and
  polyrastertest's 77 scenes), sound, the save chips, the firmware, the SPI
  devices and the clock.
- The larger picture: that a picture of whole pixels comes out as itself
  with each pixel repeated, that vertices land on the finer grid, that any
  number of threads draw the same thing, that every line the 2D tests
  render over the 3D layer stacks the same way a second time, and that a
  machine with a larger picture beside it is bit for bit the machine
  without.
- Test ROMs: armwrestler and rockwrestler pass every row. Of 71 judged
  runs of the BlocksDS hardware tests, arm7wrestler and two window tests,
  70 pass (`tools/nds/run_nds_tests.py`, baseline in
  `tests/nds/blocksds_baseline.json`); the one failure is debug output the
  runner does not print.
- The recompiler: `tools/nds/recomp_suite.py` profiles, recompiles and
  builds every test ROM and requires the interpreter's verdict and final
  state and a clean `--differential` (103 of 103 runs). Also the analyzer's
  rules on synthetic images, and `ndsrecomp` end to end on two ROMs the
  repository builds itself (`tests/fixtures/make_nds_save_rom.py`).
- The frontend: the input layer's unit test (layouts and touch mapping on
  the displays in use, bindings) and the menu self-test, which drives the
  menu with real keyboard, mouse and controller events.

The recompile suites build projects, so they need the compiler's
environment; the test ROMs are built from source in Docker by
`tools/nds/build_test_roms.py`.

Phantom Hourglass is checked against DeSmuME where an independent picture
matters: the first 250 frames are pixel-identical, and the 3D intro matches
at a constant offset. `--differential` is not an independent check: both
sides share the same devices. It proves that recompiled code does what the
interpreter does.

## What is not done

- **Other games.** One commercial game has been played. Expect missing
  hardware behaviour elsewhere.
- **Wi-Fi and local wireless.** There is no network hardware: a game's
  Wi-Fi set-up screen waits forever, and multiplayer does not exist.
- **Save states.**
- **DSi hardware, the GBA slot** (rumble, slot-2 extras), and booting
  through the real firmware menu.
- **The replacement BIOS** differs from Nintendo's in a few calls a game
  could notice: the sign of `Div`'s remainder, `Sqrt` of large values, the
  decompression calls that take callbacks, `SoftReset`. They are listed in
  the plan. A user's own BIOS dumps can be given with `--bios9` and
  `--bios7`.
- **Timing** is approximate where the ARM9's memory accesses are concerned;
  Phantom Hourglass runs its prologue a few hundred frames ahead of
  DeSmuME by the end.
- **The microphone** is tested as a device and a binding, and not yet at a
  point in a game that needs it. The lid has been checked in Phantom
  Hourglass as far as sleep and wake.
- **The clock** starts at this computer's time and then follows emulated
  time: it stops while the menu is open and runs ahead under fast forward.
- **Not tried by hand:** a real controller, a real touch screen and this
  computer's microphone (the self-test uses virtual ones).
- **Linux, macOS and the exported builds for them** wait until the Windows
  build has been played.
- **Internal resolution, where it does not reach.** A captured picture is
  drawn larger only when it is shown as it was taken: as a direct-colour
  bitmap background or bitmap sprites, unscaled and unflipped. One used as
  a texture on a polygon, scaled, or blended with another as it is
  captured (a motion blur) is the console's own pixels. The outlines of
  edge marking are one larger pixel wide, so thinner than the console's at
  the same size on screen.
- **Speed.** Between two and three times real time headless on the
  development machine; the executable is large (129 MB for a 64 MiB ROM)
  because each instruction is a call into a shared handler.
