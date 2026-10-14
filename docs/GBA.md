# Game Boy Advance (in progress)

The hub recompiles and plays `.gba` ROMs. Every test ROM we have runs the same
recompiled as interpreted, in cycle-exact lockstep. The Legend of Zelda: The
Minish Cap (USA) builds, boots, and plays through the menus, file creation
(EEPROM save), the intro and into the game at 99.9 % native code, clean in
`--differential` over 6,000 frames of scripted play. The plan, phase gates, test strategy and current
results are in [docs/GBA-PLAN.md](GBA-PLAN.md).

## Layout

```text
runtime-gba/      ARM7TDMI core (interpreter + shared instruction handlers),
                  machine (memory, I/O, timers, DMA, IRQs, PPU, APU, saves,
                  HLE BIOS), recompiled-code dispatcher, headless runner,
                  SDL2 frontend (platform/)
recompiler-gba/   gbarecomp: analyzer and C emitter
tests/gba/        SingleStepTests harness, EEPROM test, test-ROM gates
tools/gba/        test-ROM runners, recompile suite, frame tools
```

## Using it

From the hub: import a `.gba` ROM. It builds like a Game Boy game; the
installed folder holds `<id>.exe`, `SDL2.dll` and the battery save
`<id>.sav`. The ROM is compiled into the executable and no BIOS file is
needed (the few BIOS calls games make are built into the runtime). Hub builds
profile the game first (see below; about 20 s) and keep the trace in the
game's `profile/` folder. Any other `*.txt` trace put in that folder is used
by the next rebuild too.

By hand:

```bash
./build/bin/gbarecomp game.gba -o output/game --profile output/game.profile/boot.txt
cmake -G Ninja -S output/game -B output/game/build -DCMAKE_BUILD_TYPE=Release -DSDL2_DIR=<SDL2 cmake dir>
ninja -C output/game/build
./output/game/build/game                              # window, sound, input
./output/game/build/game --frames 600 --dump-frame last.ppm
./output/game/build/game --differential --frames 300
```

Games copy code into IWRAM/EWRAM at run time, and some ROM code is only
reached through function pointers. Static analysis cannot see either, so
that code would run in the interpreter. `--profile FILE` first plays the
game with the interpreter for 6,000 frames (`--profile-frames N`) under a
built-in input script: START and A through the boot screens and menus, then
walking around and pressing the other buttons. It writes the entry points it
saw to FILE and RAM images beside it (`FILE.iwram`, `.ewram`), then recompiles
with them. RAM code is compiled from those images and checked against live RAM
before it runs natively. On Minish Cap the profile alone gives 99.9 % native
code.

Traces from longer or hand-scripted runs add to it (`--trace` is repeatable):

```bash
./output/game/build/game --frames 20000 --keys 900:START --trace-out more.txt
./build/bin/gbarecomp game.gba -o output/game --trace output/game.profile/boot.txt --trace more.txt
```

Build time: generated files hold about 4,000 guest instructions each
(`--instructions-per-file N`), and the CPU handlers and fetch/event helpers
are called out of line instead of inlined after every instruction. Minish Cap
compiles in 12 s instead of 142 s on a 20-core machine, and its native code
runs about 15% slower (still over 1,300 frames per second).
`--inline-handlers` restores inlining for the fastest code.

`--coverage` shows what is still interpreted. `--keys
FRAME:BUTTON[+BUTTON][:DURATION]` scripts input for headless runs;
`--auto-input` plays the profiling script instead.

Cartridges with a real-time clock (Pokémon Ruby, Sapphire and Emerald,
Boktai; found by the `SIIRTC_V` library string or the game code) get one on
the GPIO port. While playing it shows your computer's local time. Time the
game sets is kept as an offset, saved with the status register in 16 bytes
after the battery save in `<id>.sav`. Headless runs start the clock at
2026-01-01 12:00:00 (`--rtc-time "YYYY-MM-DD HH:MM:SS"`) and advance it with
emulated time, so they are reproducible.

`gba_play game.gba` plays a ROM with the interpreter alone; `gba_run` is the
headless interpreter runner.

Every control can be rebound in the settings menu (two keyboard and two
controller bindings per action; hold modifiers while choosing a key to bind
a combination). The defaults follow the hardware the build targets
(`runtime-gba/platform/gba_input.cpp`):

| Action | Windows | Mac | Steam Deck (Linux) |
| --- | --- | --- | --- |
| D-pad | Arrows, WASD | Arrows, WASD | D-pad, left stick |
| A / B | Z, J / X, K | Z, J / X, K | B / A (right / bottom buttons) |
| L / R | Q / E | Q / E | L1, L2 / R1, R2 |
| Start / Select | Enter / Backspace, Right Shift | Return / Delete, Right Shift | Menu / View |
| Fast forward (hold) | Tab | Tab | R3 |
| Save / load state | F5 / F8 | Cmd+S / Cmd+L | X / Y |
| Previous / next slot | F6 / F7 | Cmd+[ / Cmd+] | — |
| Overlay | F1 | Cmd+I | — |
| FPS counter | F9 | Cmd+Shift+I | — |
| Settings menu | Escape, F10 | Escape, Cmd+, | L3 |
| Fullscreen | F11 | Cmd+F | — |
| Screenshot | F12 | Cmd+P | — |
| Mute | M | M | — |

A Mac laptop's F-keys are brightness and volume keys unless fn is held,
hence the Command shortcuts. Linux builds are the SteamOS build: Steam keeps
the Steam and Quick Access buttons, so L3 opens the menu, and a keyboard
gets the Windows layout. Controllers use the Nintendo layout on every
platform (east button = A, south = B, shoulders and triggers = L/R); L3 and
the Guide button open the menu, and Escape always does. Buttons are named
for the controller in use (Xbox, PlayStation, Nintendo, Steam Deck). A key
bound without modifiers does not fire while Ctrl, Alt or Command is held,
so Cmd+S does not also press S (down).

The settings menu is the Game Boy runtime's, for GBA: the game pauses
while it is open. It has display (fullscreen, scaling mode, filter, window
size), speed, volume and mute, ten save-state slots, and the controls.
`gba_menu_selftest` opens and closes it with synthetic Escape, Guide and L3
presses, checks a volume set in it survives a restart, and checks that
`--mute` stays out of the saved settings.
**Show FPS Counter** (or its shortcut) puts one line in the window's top
right corner, `60 FPS  100%`: emulated frames per second and the game's
speed, 100% being full speed. It is kept as `display.fps`.
Master volume runs 0-100 and defaults to 50, as for Game Boy: the APU mixes
near full scale, so 50 plays at 2 % of the raw output and 100 at 4 %.
Display, audio, slot and control choices are kept in `gba_prefs.ini` in
SDL's per-user preference folder (`gbrecompiled/runtime-gba`); bindings are
stored only where they differ from the platform's defaults, as
`keyboard.<action>.<slot> = key:<scancode>[:<modifier bits>]` and
`controller.<action>.<slot> = button:<n>` or `axis:<n>:+`. Save states
are `<id>.state1` to `<id>.state10` next to the battery save. A state is
the whole machine and loads only into the same ROM built with the same
runtime; it includes the battery save as it was, but loading one leaves the
`.sav` on disk alone until the game saves again. The menu uses the Dear
ImGui copy in `runtime/vendor/imgui`; gbarecomp copies it into generated
projects as `runtime-gba/vendor/imgui`.

Scale filters: **Nearest** and **Linear** scale the 240x160 frame in one
step. **Sharp Linear** scales by the largest whole number that fits with
nearest-neighbour on the GPU, then smooths only the remaining fraction, so
pixels stay square and even at sizes such as 4K (13.5x). **Smooth (MMPX)**
runs the MMPX pixel-art magnifier (`runtime-gba/src/gba_upscale.c`, MIT,
McGuire and Gagiu) twice on the CPU for a 960x640 frame that redraws
stair-step edges and diagonals, then scales linearly to the window; it costs
about 2 ms per frame.

Screen looks, for big windows and fullscreen, where each GBA pixel becomes a
large block: **Soft** scales the frame 3x with nearest-neighbour (the small
window's picture) and smooths it the rest of the way, so blocks keep their
shape with soft edges. **LCD Grid** draws whole pixels with faint lines
between them, like the GBA's screen. **Scanlines** is Soft with darker lines
along the bottom of each pixel row. The lines appear once a GBA pixel is at
least 3 screen pixels wide.

**CRT** draws the picture as a CRT television shows it
(`runtime-gba/src/gba_crt.c`): each line is a glowing beam, blurred a little
along the line, that is thin where the picture is dark and swells as it
brightens, so dark areas show black gaps between lines and bright ones
nearly close them. An aperture grille of red, green and blue stripes, one
screen pixel each, is multiplied over it once a GBA pixel is 3 screen pixels
wide, and a soft halo is added around bright areas. The beams are drawn on
the CPU at whole screen rows per line (9 at 1440p fullscreen), in linear
light, and cost about 4 ms per frame there. It shows the game's own pixels,
not a texture pack's. It needs at least 2 screen rows per GBA line; smaller
windows get Linear.

Windowed options: `--bios FILE`, `--scale N`, `--save-dir DIR`,
`--fullscreen`, `--mute` (each this run only; the saved Fullscreen and Mute
settings are kept),
`--interpreter`, `--limit-frames N`,
`--screenshot-prefix P`, `--player N` and the link options (see Link cable),
`--audio-stats` (prints the audio queue depth and
how often it ran dry on exit), `--prefs FILE` (use that settings file instead
of the player's), `--load-state SLOT` (load save-state slot 1-10 at start),
`--display-shot FILE.bmp` with `--display-size WxH` (on exit, write the last
frame as the display shows it at that size, default 3840x2160, through the
saved scaling mode and filter), `--texture-pack FILE.gbatp` (high-resolution
tiles, see [TEXTURE_PACKS.md](TEXTURE_PACKS.md); the menu's *Use Texture
Pack* switches back to the original to compare), `--record-textures FILE`
(record video state for making packs, see tools/gba/texpack),
`--affine-scale N` (this run only). Headless options (`--frames`, `--until-idle`,
`--expect rN=V`, `--expect-text`, `--screen-text`, `--dump-frame`,
`--dump-audio FILE.wav`, `--differential`, `--hash-frames`, `--affine-scale N`,
`--dump-large FILE.ppm`) run without a window and never write saves. `--dump-audio` also reports DirectSound FIFO
underflows; `--hash-frames` prints one hash of every frame of the run, for
checking that a renderer change keeps the picture identical.

**Rotation and scaling resolution.** Backgrounds and sprites the console
draws through a matrix (BG2 and BG3 of modes 1 and 2, the bitmap of modes 3
to 5, rotated and scaled sprites) can be drawn with two, three or four
samples a pixel each way: Rotation/Scaling Resolution in the settings menu,
or `--affine-scale N` for one run. A turned, reduced or tilted layer loses
its staircase and keeps detail the console's 240 pixels drop. The console's
own picture is still drawn, exactly as before, and is what the game, save
states, screenshots, texture packs and the CRT filter use; a frame that had
such a layer is displayed from the larger picture, which the other filters
treat as they treat a texture pack's. Tiled backgrounds and ordinary sprites
are the console's pixels, repeated; a texel that already covers several
pixels stays a square. Not drawn larger: lines with a mosaic or green swap,
and anything while a texture pack is in use. A full-screen turned layer
costs about 0.8 ms a frame at 2x and 2.8 ms at 4x.

Audio is kept about 80 ms ahead of the sound device: frame pacing runs up
to 0.5 % fast or slow to hold that level, and catches up at once if it
drops below half. To check the device side without speakers, run with
`SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=out.raw` (raw 16-bit stereo).

## Link cable

Two to four windows of the same game can be linked like GBAs on a link
cable, on one computer or across a network, in multi-player mode (SIOCNT
mode 2: Four Swords, Mario Kart, Pokémon trades and battles, multiboot).
Each window is one GBA; player 1 hosts and the others join.

- **Hub:** a GBA game's page has *2 Players on this PC* (two linked windows
  side by side), *Host for other PCs*, and *Join* with the host's address.
  Game Boy and Game Boy Color games have the same buttons (see
  [RUNTIME.md](RUNTIME.md#link-cable)).
- **In game:** the menu's *Link Cable* section hosts or joins while playing
  and shows who is connected.
- **Command line:** `--link-host` (player 1; `--link-local` accepts this
  computer only, so Windows asks no firewall question), `--link-join
  HOST[:PORT]`, `--link-port N` (default 47400).

`--player N` (1-4) gives a window its own profile, which is what two
windows on one computer need: players 2-4 keep battery saves, save states
and screenshots in a `Player N` folder beside the game (the hub keeps it
across rebuilds) and settings in `gba_prefs_pN.ini`, read the Nth
controller (changeable in the menu's Controls section; controllers keep
working when the other window has focus), and open their windows side by
side. Player 1 uses the ordinary files, so its save is the single-player
one. Keyboard input goes to the focused window only.

How it works (runtime-gba `gba_link.c`, `gba_net.c`): the machines run in
lockstep on the parent's cycle clock. A transfer the parent starts at time
T completes at T + its wire time on every machine, a child never runs more
than 4,096 cycles (less than the shortest transfer) past what the parent
last reported, and the parent completes a transfer only once every child's
SIOMLT_SEND for it has arrived. So every machine sees exactly the data and
interrupt timing real hardware gives; a window that must wait for another
(its menu is open, or the network is slow) shows "Waiting for Player N".
Children are paced by the parent. Save states cannot be loaded while other
players are linked. Other serial modes (normal, UART, JOY bus) are not
emulated; with no cable attached the serial registers behave as before.

Latency: each transfer costs one network round trip. Four Swords makes
about 13 per frame; measured with the headless runner and
`GBA_NET_DELAY_MS` (a test aid that holds received messages back), it keeps
60 fps up to about 1 ms of round trip: fine on one computer or wired LAN,
Wi-Fi may slow the game down. The headless runner takes the same link
options plus `--link-wait N` (start once N players are linked), for
scripted two-process tests.

## Tests

```bash
python tools/fetch_gba_tests.py            # pinned test suites into roms/gba/
ctest --test-dir build -R gba              # SST vectors, ROM baseline, recompile, EEPROM
python tools/gba/run_gba_tests.py          # jsmolka + Alyosha R12 ROMs, interpreter
python tools/gba/recomp_suite.py           # same ROMs recompiled, plus --differential
```

`gba_ppu_large` checks the larger picture of rotated and scaled layers
against the console's formulas in exact arithmetic, sample by sample: tiled
and bitmap backgrounds, a matrix set every line, and a sprite, at 2x, 3x
and 4x, with the console's frame unchanged by the scale.
`test_gba_ppu_large --time` prints what a frame costs at each scale.

## What is not done

- Only one commercial ROM (Minish Cap) validated so far.
- The Game Boy menu's audio device picker has not been carried over.
- Speed: headless Minish Cap runs at about 1,790 fps recompiled and 1,180
  fps interpreted (Release; 30x and 20x real time). Most of a frame goes to
  the scanline renderer; the per-instruction overhead of generated code
  (fetch cycles through the prefetch model, and the yield check after every
  instruction) has not been optimised.
- Code first reached late in a game (after the profile's 6,000 frames) runs
  interpreted until a longer trace is supplied.
- Cycle timing: DMA/IRQ interplay, interrupt and halt timing, and serial
  (65 of the 138 R12 ROMs fail, the same in the interpreter and recompiled
  code). The prefetch buffer and the timer start/stop delays are modelled;
  `prefetcher_branch_thumb_arm_3` is still 4 cycles short.
- Link cable modes other than multi-player; link play over high-latency
  networks (see Link cable).
- RTC interrupts (alarm, per-minute), the solar sensor (Boktai)
  and tilt sensors. No Pokémon ROM has been run yet; the clock is tested
  with the protocol its library uses (`gba_rtc_protocol`).
- The BIOS sound-driver calls.
- The mGBA suite (needs a devkitARM build) and a UBSan run of the generated
  test ROMs (the SingleStepTests harness is clean under it).
