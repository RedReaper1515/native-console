# Recorded runs

A run that somebody played, kept so that it can be played again with nobody
at the controls, to the cycle: to record what the game did with its data
(`--data-use`), which code ran (`--evidence`), or anything else a recorder
in the game can write down, over hours of real play instead of a script.

This is for the Game Boy and Game Boy Color. The file format is written so
that other consoles can use it; none does yet.

## Why a run can be had again

A Game Boy has nothing random in it. What a game does follows from four
things:

- the ROM;
- what was in the cartridge's memory when it was switched on (the save);
- where the cartridge's clock stood, if it has one;
- the cycle each button went down and came up.

A game's own random numbers come out of those: the timer, and when the
player pressed. So there is no seed to set. The run's start and its buttons
are the seed, and a recording keeps exactly them.

## Recording

In the hub, a built Game Boy game's page has **Record a run**:

- **New run** starts the game from a new save and records. The player's own
  save is not used and not touched.
- **Continue run** starts another sitting of the newest run, from the save
  that run left.
- **Runs folder** opens where they are. To hand a run on, send the whole of
  its folder.

A line in the window's top left corner, `REC segment 2  1:04:33`, says the
run is being kept. The hub keeps runs in `<data folder>/runs/<game>/`,
outside the game's own folder, so that rebuilding the game leaves them.

By hand:

```bash
./output/game/build/game --record-run output/runs/my-run
```

The folder is made if it is not there, and is where the game keeps its save
while it records. Each time the game is started with the same folder it adds
a segment:

```text
my-run/
  segment-0001.run   the first sitting: its buttons and its checkpoints
  segment-0001.sav   the cartridge memory that sitting began with
  segment-0002.run   the second sitting
  segment-0002.sav   what it began with: what the first one left
  game.sav           the game's save, as it stands
```

While a run is recorded:

- pausing in the menu, fast-forward, the speed setting and making a save
  state change nothing: buttons are kept by the game's clock, not the wall's;
- loading a save state is refused, and so is the link cable: the run would
  go on from somewhere its record does not lead;
- every line is on the disk when it is written. A game that is killed, or a
  machine that goes down, loses nothing that was played; the segment has no
  last line, and replays to where it stops.

A run cannot be recorded with `--save-dir`, `--load-state-file`,
`--benchmark`, `--no-audio` or `--differential`.

A script can play the run instead of a person: what `--input` presses is
buttons like any other, goes into the record on the cycle it reaches the
game, and replays with no script:

```bash
./output/game/build/game --headless --record-run output/runs/scripted \
  --input "c2106720:A:280896,c2808960:S:280896" --limit-frames 600
```

A program can play it too, one that looks at the game's memory before it
presses (`--control`, `tools/gb_control.py`; `RUNTIME.md`, *A program that
plays*). What it holds reaches the game at the start of a frame and is
recorded like any other button, so the run replays with no program there.
The program can read all it likes while a run is recorded, and have a save
state written. A byte written into the machine or a state loaded would not
be in the record, and the game refuses both until the recording is over.

Leave `--no-audio` out of such a command, and out of a replay. It takes the
sound hardware away, the sound registers then read `$FF`, and a game that
looks at one plays another run from there: Pokémon Red keeps the volume
register in its memory when a cry plays, `$77` on a Game Boy and `$FF`
without the hardware. A record is of a whole Game Boy, so `--no-audio` and
`--benchmark` are refused both when a run is recorded and when one is
replayed. `--headless` alone gives no sound and leaves the machine whole.

A run can be played with a pack of unpacked data (`--unpacked`,
`DATA_FILES.md`). A pack that only says what to watch changes nothing, with
`--unpacked-harvest` or without. A pack that replaces something puts other
bytes into the game's memory than the game made, so the run is a run of the
game with those replacements: the record says how many they were and a hash
of them, and it replays with a pack that has the same ones and with no
other.

## Playing it again

```bash
python3 tools/run_record.py info output/runs/my-run
python3 tools/run_record.py verify output/runs/my-run --game output/game/build/game
```

`info` lists the segments: when each was started, how long it is, whether it
was closed, and what save it began with (a blank one, the save an earlier
segment left, or one from outside the run).

`verify` plays every segment again, side by side, and says whether each
arrives where the recording did:

```text
segment 1: 48 of 48 checkpoints reached, 330 of 330 inputs, 0:00:47 played, ended where the recording did
segment 2: 27 of 27 checkpoints reached, 183 of 183 inputs, 0:00:27 played, ended where the recording did
the run replays
```

The game has to be built from the ROM the run was played on. It compares the
SHA-256 in the record with its own and says so if they differ, so a run
played on another dump of the game is found out before it is replayed. It
does not have to be the same build: see below.

One segment, by hand:

```bash
./output/game/build/game --headless --replay-run output/runs/my-run/segment-0001.run
```

A replay starts from the segment's own `.sav` and clock, never from a save
beside the game, and writes no save. Without `--headless` it plays in a
window at the speed it was played; the corner says `REPLAY`. It takes its
buttons from the record and from nowhere else, so `--input` is refused with
it, as `--no-audio`, `--benchmark` and `--differential` are.

A run played with a pack that replaces something is replayed with that pack.
`info` says so under the segment, and `verify` gives the game whatever
follows `--`:

```bash
python3 tools/run_record.py verify output/runs/my-run --game output/game/build/game \
  -- --unpacked output/data/game.gbup
```

The game looks at the pack before it replays anything. Given none, or one
that replaces something else, it says what the record was played with and
what the replay has, and does not start:

```text
[RUN] the record was played with a pack of unpacked data that replaces what the game unpacks; give the replay that pack (--unpacked):
[RUN]   record 2 replacements, 83e3cca39bb52fe4...a4be6fe9
[RUN]   replay nothing replaced
```

It is the same the other way about: a run played with nothing replaced is
not replayed with a pack that replaces something. The pack does not have to
be the same file. The hash is of what is replaced and by what: for each
replacement its bytes, the stream it is for, the hash of what the routine
makes of that stream, and where the routine is entered, finds its stream and
leaves its output. What a routine is called, the routines a pack only
watches and the order a pack lists things in are not in it.

## Recording something over a run

A replay is the game running, so every recorder the game has works over it:

```bash
./output/game/build/game --headless --replay-run output/runs/my-run/segment-0001.run \
  --data-use output/data/segment-1.json
```

A long run is replayed in pieces, side by side. `verify --states` writes a
save state every so many frames; `sweep` starts one replay from each state,
runs it to the next, and gives each the options after `--`, with `{out}`
replaced by a path of the piece's own:

```bash
python3 tools/run_record.py verify output/runs/my-run --game output/game/build/game \
  --states output/runs/my-run-states --state-frames 3600

python3 tools/run_record.py sweep output/runs/my-run --game output/game/build/game \
  --states output/runs/my-run-states --out output/data/my-run \
  -- --data-use {out}.json

python3 tools/gb_data.py uses output/data/game output/data/my-run/*.json
```

What the game unpacked while the run was played is had the same way
(`DATA_FILES.md`). For a run played with nothing replaced the pack has to be
one that replaces nothing, which is what `gb_data.py build` writes for a
tree with no unpacked picture edited, and what `gb_data.py watch-pack`
writes from the tree as it stands, whether pictures in it have been edited
or not:

```bash
python3 tools/gb_data.py watch-pack output/data/game -o output/data/game.watch.gbup

python3 tools/run_record.py sweep output/runs/my-run --game output/game/build/game \
  --states output/runs/my-run-states --out output/data/my-run-unpacked \
  -- --unpacked output/data/game.watch.gbup --unpacked-harvest {out}.json

python3 tools/gb_data.py unpacked output/data/game output/data/my-run-unpacked
```

The pack `gb_data.py build` writes beside the image is that pack only while
no unpacked picture is edited. From then on it replaces what the game
unpacks, and a run played with nothing replaced is not a run of that game:
a replay with it is refused before it starts.

Each piece checks itself against the record's checkpoints as it goes.

## How a replay is kept exact

- **Buttons by cycle.** The recorder is asked just before the machine runs
  on, each time: it writes the buttons down, with the machine's cycle count,
  when they are not what they were. A replay stops the machine on that cycle
  and changes them there. It does not wait for the place where the game's
  loop looks for input, so a replay does not depend on how the loop is cut
  (**Smooth Slow Frames**), on how fast the machine was, or on whether the
  code is recompiled or interpreted. If a build cannot stop on the cycle,
  the replay gives the input at the next place it can and counts it (`late`
  in its result line); `verify` reports any.
- **The joypad interrupt.** A press can ask for the interrupt and be let go
  before the game ever reads the buttons. The record marks the cycle
  (`!`), and the replay asks for it there.
- **The start.** The cartridge memory is copied into the run's folder as the
  sitting begins, and the clock chip's registers are written into the
  record as they stand after the time away has been added. The replay puts
  both back; the time of day has no say in it.
- **Checkpoints.** Once a second of game time (every 60 frames) the recorder
  writes a hash of the machine. The replay has to be at the same cycle with
  the same hash at each, and stops at the first it is not, naming the frame.
- **One machine, recompiled or interpreted.** Generated code runs every
  instruction for the cycles the interpreter runs it for and stops between
  the same two instructions, so a run recorded one way replays the other
  way. It takes no shortcut through a group of instructions: the one it had,
  for a loop that waits for a line of the screen, is gone (below).

The hash covers the registers, the cycle and frame counts, all of the work
RAM, the high RAM, the cartridge's memory, its bank registers and its clock.
It leaves out video memory, the sprite table and the hardware registers: the
runtime brings some of those up to date only when the game looks at them, so
two builds can hold them differently at the same cycle without the game
being able to tell.

## What a recording does not survive

- **A change to the runtime's timing.** A record is the buttons by the
  runtime's count of cycles. A fix that makes an instruction, an interrupt
  or the screen take a different number of cycles moves everything after
  it. `verify` says at which frame a run leaves its record. The states
  `verify --states` wrote with the older runtime are from before the change
  and still start the game where the run was, so what was recorded over the
  run does not have to be thrown away; replaying past that point does.
  Keep the commit a run was verified at beside it.
- **A wait for a line, in a game generated before October 2026.** Generated
  code passed the loop `ldh a,[rLY]` / `cp r` / `jr nz` in one step, and
  left it one turn of the loop (28 cycles) later than the instructions do
  whenever the line began between the start of a turn and the turn's read of
  LY. The interpreter never did. A run such a game recorded replays in that
  game; in its interpreter (`--data-use`), in lifted source and in a game
  generated since, it leaves its record at the first wait that went so
  (Pokémon Red has the loop on its title screen and in its credits). The
  states from before that frame and from after it still start the game
  where the run was.
- **Another ROM.** Not the same bytes, not the same run.
- **Data mods.** The record notes how many overlay entries were active and a
  replay with another number is refused.
- **Other replacements of unpacked data.** The record notes how many
  replacements the pack given with `--unpacked` had, and a hash of them. A
  replay with others, or with none where there were some, is refused before
  it starts. A record from before this was noted has no `unpacked` line: it
  is replayed with whatever pack it is given (where that pack replaces
  something, the game says that it cannot tell), and the checkpoints find a
  replay that is not the run.
- **A machine with a part left out.** A build from before `--no-audio` was
  refused could record with it. Such a record is of a Game Boy without
  sound hardware: on a whole one it replays as far as the first sound
  register the game makes use of, and no further. Record the run again.

## The file

Text, a line each, so that it can be read, compared and cut by hand. The
header:

```text
native-console-run 1
console gb
segment 2
rom 5ca7ba01...96b7b 1048576 POKEMON RED
model dmg 0
clock 4194304
checkpoint-frames 60
save segment-0002.sav 32768 e8fe2122...ac01 used
rtc 40 46 3 1 0 40 46 3 1 0 0 1183744 1
data-mod 0
unpacked 0 -
started 1791642588
begin
```

| Line | What it says |
|---|---|
| `native-console-run 1` | The format and its version |
| `console` | The console; what the `i` lines hold is the console's own |
| `segment` | Which sitting of the run |
| `rom` | SHA-256, size, and the title in the cartridge header |
| `model` | `dmg`, `cgb` or `sgb`, and 1 for a CGB running a DMG cartridge |
| `clock` | Cycles in a second, for lengths |
| `checkpoint-frames` | Frames between checkpoints |
| `save` | The file beside this one that holds the cartridge memory the sitting began with, its size and SHA-256, and `blank` if every byte is the same; `save -` for a cartridge with no memory |
| `rtc` | With a clock chip: seconds, minutes, hours, days low, days high, the same five latched, the latch state, the cycles into the current second, and whether it is running |
| `data-mod` | Data-mod entries active |
| `unpacked` | How many replacements the pack of unpacked data had (`--unpacked`), and the SHA-256 that tells them from any others; `0 -` with no pack, and with one that only watches |
| `started` | The time of day, for people. Nothing reads it back |

A reader skips header lines it does not know. So a line added later has to
be one that a game built before it can do without. `unpacked` is: such a
game replays the record with the pack it is given, as it replays any other,
and with the wrong pack leaves the record at the first checkpoint after a
replacement. It only cannot say why. After `begin`:

| Line | What it says |
|---|---|
| `i CYCLE BUTTONS [!]` | The buttons held from this cycle on: letters `U D L R A B`, `S` for Start, `T` for Select, as `--input` has them, or `-` for none. `!`: the joypad interrupt was asked for |
| `k FRAME CYCLE HASH` | A checkpoint |
| `e CYCLE FRAME HASH SAVE-SHA256` | The game was closed here, with this in the cartridge's memory |

`--record-input` and `--input` are still there, for short scripted
repros: `--input` holds 2,048 entries and carries no start, and a played run
needs both.

## Another console

What is the console's own: the `console` name, the `model` line, the
contents of `save` and `rtc`, what an `i` line holds (a pad with sticks
needs numbers, and a line for each controller port), and the hash. The
rest (segments, a start kept beside the record, inputs by the machine's own
clock, checkpoints, `e`, the tool) is the same.

Before a console's runtime can record runs it has to pass the test this one
does: the same run replayed twice, and replayed with its loop cut another
way, ends in the same state. Where a runtime reads the wall clock, a thread
or the audio queue while it runs, that is what has to go first. And an
option that leaves a part of the machine out which the game can read (a
mute that stops the sound chip being emulated, a reduced mode for
measuring speed) has to be refused with a run, recording and replaying.
