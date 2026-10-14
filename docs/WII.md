# Wii

> **Not playable yet.** Games build and may start, but expect wrong
> graphics, missing sound and crashes. The hub marks Wii games the same way.

Status: **in progress, not yet supported.** The machine, the disc reader, the
recompiler path and the hub backend exist and pass their tests against
programs and a disc built in this repository. One retail game has been
tried: Super Mario Galaxy 2 boots, connects a remote, reaches its title
screen and its file select, creates a save file, tells its opening story
and starts its prologue with Mario in play, recompiled, from a script of
button presses. It has sound: forty seconds of its title music are the
same to the bit as Dolphin's. On the fast OpenGL renderer it holds a
console's speed through all of that but the two places it loads; nobody
has played it by hand.
[docs/WII-PLAN.md](WII-PLAN.md)
has the phases, the gates and what is open.

The Wii is the second console of the GameCube family. It shares
`runtimes/runtime-gc/` and `gcrecomp` with the GameCube and adds the files named
`wii_*`; everything in [docs/GC-PLAN.md](GC-PLAN.md) about the
processor, the graphics chip and the renderers applies to both.

## What you need

- **A disc image you own**, as `.iso`, `.wbfs` or `.ciso`. RVZ, WIA and GCZ
  are not read yet: convert with Dolphin (right-click a game, Convert File).
- **Your console's common key.** A retail disc is encrypted and the key is
  not in this repository or its releases. Supply it once, any of these ways:
  - the hub's Settings page, "Wii common key file";
  - `gcrecomp --key-file FILE`;
  - the environment: `WII_COMMON_KEY` (32 hex digits) or `WII_KEY_FILE`;
  - a file named `wii-common-key.bin` or `keys.bin` beside the image.

  The file may be the 16 bytes, 32 hex digits of text, or a `keys.bin` /
  OTP dump made on your console with BootMii. A wrong key is reported as
  wrong; it never reads as garbage.

  The key is needed to *build* a game. The built game carries the key to its
  own disc (the title key) and needs nothing more.

## In the hub

Wii is listed as its own console and stays switched off until a retail game
has been through the gates. To try one now, turn on "Try Wii games" in the
hub's Settings (or start the hub with `NATIVE_CONSOLE_WII=1` in the
environment). Without it a Wii disc is catalogued and not built; a disc
that was refused can be built with Rebuild once it is on.

A built game's folder holds the executable, `ID.gcdata` (the disc, as a hard
link to the library's copy where the volume allows), `ID.nand` (the console's
flash memory: saves and settings) and, if the game uses one, `ID.card` (a
GameCube memory card).

## By hand

```bash
ninja -C build gcrecomp gc_run gc_play

# Interpreted, headless or in a window. The disc says which console it is.
./build/bin/gc_run "game.wbfs" --frames 600 --dump-frame out.ppm
./build/bin/gc_play "game.wbfs"

# Recompiled.
./build/bin/gcrecomp "game.wbfs" -o output/game --name game
cmake -G Ninja -S output/game -B output/game/build -DSDL2_DIR=...
ninja -C output/game/build
cp "game.wbfs" output/game/build/game.gcdata   # or a hard link
./output/game/build/game
```

The headless runner's options are the GameCube's (`runtimes/runtime-gc/include/
gc_runner.h`): `--frames`, `--dump-frame`, `--differential`, `--coverage`,
`--trace-out`, `--input`, `--auto-input`. On a Wii, scripted input drives the
first remote (with a Nunchuk) through the same names, and the script's
second stick is where the remote points: the middle of the screen at rest.

## Controls

F9, or **Show FPS Counter** in the menu, shows the fields per second and the
game's speed (100% is full speed) in the window's corner, for GameCube games
too.

The remote's buttons are the GameCube actions under other names, so one set
of bindings (F10, Controls) serves both consoles. "Played with" in that menu
chooses what they mean:

| Played with | A / B | X / Y | Start | Stick | L / R | D-Pad |
|---|---|---|---|---|---|---|
| Wii Remote + Nunchuk | A / B | 1 / 2 | + | Nunchuk stick | Z / C | D-Pad |
| Wii Remote, sideways | 2 / 1 | A / B | + | D-Pad (turned) | | D-Pad (turned) |
| Classic Controller | a / b | x / y | + | left stick | L / R (Z is ZR) | D-Pad |
| GameCube Controller | the game is played on a GameCube pad in port 1 | | | | | |

The mouse is the pointer wherever the remote has one, and its two buttons are
A and B; the right stick moves the pointer too. Minus (`-`, or Back on a
controller), Home (`Home`) and a shake (`Space`) have bindings of their own.

## Picture

A Wii game is shown 16:9, as on a widescreen television: "Widescreen (16:9)" in the same menu
(F10, Controls) is on unless you turn it off, and `--wii-aspect 4:3` or `16:9` chooses for one
run. It is the console's own setting, which a game reads when it starts, so a change takes
effect the next time the game is started; a save state keeps the shape it was made in. The
other picture settings - renderer, internal resolution, scale filter - are the GameCube's
(F10, Graphics).

## Tests

```bash
python tools/wii/build_wii_programs.py     # needs devkitPPC (DEVKITPRO)
cmake -G Ninja -B build . -DBUILD_TESTS=ON # again, so the program tests register
ctest --test-dir build -R "^wii_"
```

`docs/WII-PLAN.md` lists what each test holds. None needs a game or a key:
the disc they read is built around keys made up for it.

## Not done

- A retail game past its title screen. Sound for Wii games, MotionPlus, the remote's speaker, the
  Balance Board, RVZ/WIA images, network, SD cards, USB devices.
- Linux and macOS (the owner's Windows-first rule).
