# The ROM's data as files

A generated Game Boy game has its ROM built in, as one array of bytes. Its
code has been turned into C; everything else in the ROM (text, graphics,
maps, tables, music) is still in that array, and to change any of it meant
patching bytes at an offset.

`tools/gb_data.py` takes the ROM apart into files, one for each region, and
puts it together again. Put together unedited, it is the original ROM, byte
for byte, and the tool checks that every time. Put together with a data file
edited, it is an image the game loads in place of its own ROM.

This is the bottom layer of making a game's data editable: it knows nothing
about what a region holds. Turning regions into pictures, text and tables
that can be edited as such is built on top of it, and each of those steps
has to keep this one true: unedited, the image is the original. Two of them
are built: uncompressed tiles as PNG pictures, and text as text; tile maps
are a third, as grids of tile numbers. What the
game is seen to do with its data also goes back to the analysis, which then
leaves more of the data alone. And data that lies packed in the ROM is
edited as what the game's own routine unpacks it to, which leaves the image
alone altogether. A string made longer is moved into free space, where
every pointer that leads to it is data. Music and sound are lists of
commands for the game's sound engine, and are edited as those commands,
from a declaration of what the engine's commands are. And where a number in
RAM comes from (a formula, its tables, its constants) is asked of a played
game from the other end, by tracing a write back.

One command does all of it for a ROM, from generating the game to a tree
with every one of those views in it: the next section. The sections after it
are the steps one at a time, which is what that command runs and what there
is to know when one of them finds less than was hoped.

## The way in: one command

Every section below is a step with a command of its own, and a tree with
everything in it takes a dozen of them in the right order, with the same
options said again each time. `tools/gb_workspace.py` runs them:

```bash
python3 tools/gb_workspace.py game.gb output/work/game
python3 tools/gb_workspace.py game.gb output/work/game --states saves/ --sym game.sym -- --pointer-scan strict
```

It prints each command before it runs it, so the steps can be learnt from it
and done by hand, and it leaves in the folder:

| | |
|---|---|
| `README.txt` | What is there, what was found, what was not and why, and the commands that build an edit and start the game with it. |
| `tree/` | The data tree. The rest of this document is about what is in it. |
| `game/` | The game, generated and built: `game/build/game`. |
| `export.json`, `game.annotations` | The analysis the tree was split from, and what it was told is data. |
| `records/` | What the game did with its data, a record for each piece of play. |
| `harvests/`, `called/`, `watch.gbup` | What the game's unpacking routine made: in play, and of the tables declared. |
| `game.gb`, `game.gbup` | The image and the pack the tree builds to. |
| `names.txt` | Everything the tree has, with its address, its name and its file. |
| `old/tree-N` | The tree as it was before it was split again. |
| `workspace.json`, `workspace.log` | What the folder was made from, and every command with what it said. |

**What else there is** turns steps on. None of it is needed, and any of it
can be given later:

| Given | What it turns on |
|---|---|
| `--states FOLDER` | Play recorded from each save state in the folder (`*.state`): 900 frames (`--frames N`) with buttons pressed at random, and `--rounds N` times with other buttons. |
| `--run FOLDER` | A recorded run (`RUN_RECORDS.md`) played again in pieces of a minute with the recorder on: `run_record.py verify --states`, then `sweep`. |
| `--charmap FILE` | `text`: the strings as text. Without one, `glyphs` draws the sheet of the game's letters and writes a charmap to fill in. |
| `--unpackers FILE` | The pack that only watches, the same play again with `--unpacked-harvest`, `unpack --tables` for each unpacker that declares tables, and `unpacked`. |
| `--sym FILE` | `names --sym`, repeatable. With or without one, a file that has no name is called by what the game used it as (`names --seen`; `--no-seen-names` leaves the address). |
| `--annotations FILE` | Lines of one's own for the analysis (`function`, `label`, `data`). |
| `-- OPTIONS` | What `gbrecomp` is given. The game and the export are always made with the same ones. |

With no play given, the game is recorded from switching on: 1,800 frames
(`--boot-frames N`), once with nothing pressed and once with buttons pressed
at random. Those two are recorded in any case. They show logos and a title
screen and seldom more, and everything in the tree but the files themselves
is found from what the records show. Save states from all over the game are
what makes a tree worth having, and the README says so when there are none.

**The steps, and the two passes.** What is code is what the analysis decoded,
and it decodes stretches of data as instructions until it is told otherwise
(*Telling the analysis what is data*). So the game is made twice:

1. `gbrecomp ROM OPTIONS -o game`, cmake and ninja, then `gbrecomp ROM OPTIONS
   --lift-export export.json` and `split`: a first game and a first tree.
2. The game played, each piece with `--data-use FILE --data-use-tiles`, then
   `uses` and `annotations` with the folder of records.
3. If that says anything the game was not made with, the game and the export
   again with `--annotations game.annotations`, and `split --keep`: the new
   export's tree with everything the old one holds (*A tree split again*). It
   is written beside the tree and then takes its place, and the old tree is
   kept as `old/tree-N`. `uses` again, since the new tree has data the old
   one had as code.
4. `names`, then `tiles` with no file named, `maps`, `text` or `glyphs`, the
   pack, the harvest, `unpack`, `unpacked`, `names` once more for what was
   unpacked and for the list, and `build`. Names come first so that each
   picture is made under the name it keeps.

`game.annotations` is what the records showed (`seen.annotations`, as
`annotations` wrote it) and the lines given with `--annotations`. `gbrecomp`
takes one file and refuses two lines that share a byte, so what the records
showed is cut around every line given: those are the ones somebody vouches
for.

**Run again.** The same command on the same folder, or `gb_workspace.py ROM
DIR` with something more. What is not said again is as it was. A step is done
again only when what it was made from has changed: the ROM, `gbrecomp`, the
runtime's sources, the options, a file that was given, a save state, the
records. So a run with nothing new runs no command, more save states record
only the new ones, and a charmap given later runs `text` and what follows it
and builds no game. A run that stopped goes on from the step that failed:
the message says which command stopped and with what, and `workspace.log` has
all of it. Records only add up. A record of a save state that has been taken
out of the folder stays, since what it showed is still true of the ROM, and
taking it away would tell the analysis less than the tree was split with.

**The tree may have been edited in between,** and no step writes over an
edit:

- an edit starts no step, since the tree is what the steps make and not what
  they are made from;
- the tree is split again only with `--keep`, never with `--force`, and the
  tree it replaces is kept. An edit to bytes that the new analysis has as
  code stops the run with `split`'s own message; `--lose-edits-in-code` is
  passed on;
- `text --keep-edited` leaves a file of text that was edited as it is and
  writes the others, where `text` alone refuses all of them; `tiles`, `maps`
  and `unpacked` leave an edit alone by themselves;
- a charmap or an unpackers declaration is copied into the tree while the
  tree has none, an empty one, or the one that was copied before. One that
  was worked on in the tree is the tree's own, and the tool says that it
  stays.

**What it needs.** `gbrecomp` built (`--gbrecomp PATH` if it is not in
`build/bin`), and cmake, ninja and a C compiler on `PATH`. It looks before it
makes anything, whenever the run may build a game, and says which is missing.
`--cmake-arg ARG` goes to the game's configure (`-DSDL2_DIR=...`), `--workers
N` is how many copies of the game run at once (4, at most 8), and `--timeout
SECONDS` what one of them may take: a game whose screen is off never ends a
frame. A piece of play that left no record is named in the README with what
the game said, and is not tried again until `--retry`.

**Save states made with `--no-audio`.** A state has the sound hardware in it
or has not, and a game loads it only when it is run as the one that made it
was. The tool reads that off the state's header and runs such a state's
pieces with `--no-audio`, and the README counts them: the sound registers
read `FF` there, which a game can tell. `gb_data.py unpack` does the same
with its `--state` now; it used to run every game with `--no-audio`, and a
state made by a game that had its sound did not load.

What this does not do:

- **Play.** Buttons at random find a menu and a first room. They do not
  finish a game. The tree has what the play it was given shows.
- **Anything a person has to read first.** It writes no charmap, proposes no
  unpacker (*Finding the routine that unpacks* needs an instruction to
  follow), and takes no guess: `unpack --walk` is left to do by hand, since
  what it finds past the last known stream is to be looked at.
- **Room for what grows.** The records are made without
  `--data-use-pointers`, and `pointers` and `free` are not run (*Data that
  grows, by moving*).
- **New strings in a file of text that was edited.** The file stays as it
  was left, with the strings it had.
- **Pictures drawn again when a declaration's `as` changes.** `unpacked`
  leaves what is in the tree alone; `unpacked --force` draws again, and loses
  the edits.
- **Lifted source, a launcher of several games, or the hub.** A folder is of
  one ROM, `--lift` is refused, and the hub's *Make data tree* job does not
  call this yet.
- **Keep to itself when the tools change.** A newer `gbrecomp` or runtime
  makes the game again, and a tree whose export came out otherwise is split
  again. Edits are kept as above, and the time is spent.

## A first run

```bash
# 1. Describe the ROM, with the options the game is generated with.
./build/bin/gbrecomp game.gb --pointer-scan strict --lift-export output/lift/game.json

# 2. Apart: one file for each region, and a manifest.
python3 tools/gb_data.py split output/lift/game.json --rom game.gb -o output/data/game

# 3. Together again. Unedited, this is the ROM; --expect-original makes that the exit code.
python3 tools/gb_data.py build output/data/game -o output/data/game.gb --expect-original

# 4. Edit a data file (keeping its size), build again, and play it.
python3 tools/gb_data.py status output/data/game
python3 tools/gb_data.py build output/data/game -o output/data/game.gb
./output/game/build/game --rom-image output/data/game.gb
```

## What the files are

`split` reads a lift export (`LIFTING.md`), which lists every run of ROM
bytes that no decoded instruction covers. Each such run is a region. So is
each run of code between them, cut where a bank ends, and the cartridge
header. Every byte of the ROM is in exactly one region:

| Kind | What | Can it be edited |
|---|---|---|
| `data` | A run of bytes no decoded instruction covers. Named after the export's guess at it: `unknown`, `padding`, `ascii_text`, `pointer_table`. | Yes, keeping its size. |
| `code` | A run of decoded instructions. | No. The game's C was compiled from these bytes. To change code, lift the function and edit the C. |
| `header` | The cartridge header: what kind of cartridge this is, and how much ROM and RAM it has. | No. |

A file is `bank_BB/AAAA_kind.bin`: the bank and the address its first byte is
mapped at. `manifest.json` (`gbrecomp.data-tree`, version 1) lists the regions
in the order they lie in the ROM, each with its offset, size, bank, address,
kind, file, and the SHA-256 of what `split` wrote. For a data region it also
has the export's `guess`, and `referenced_by`: the functions that load a
pointer into the region, which is where to start when working out what it is.

`build` reads each file, and stops if one has changed size, if a `code` or
`header` file is not what `split` wrote, or if the manifest's regions do not
cover the ROM exactly. It writes the image and says whether it is the
original ROM (by SHA-1) or which files were edited. `status` says the same
without writing anything.

The files are the game's data. Keep them out of the repository, as the ROM is.

## The game and the image

`--rom-image FILE` makes a generated game use the image in place of the ROM
built into it. The executable is not changed: without the option it is the
game as it was.

The hub does this from a game's page: *Edited data* there takes a data tree,
which it builds before every start, or an image already built
(`hub/README.md`, *Edited data*).

The game checks the image itself, whoever made it. It has to be the size of
the game's ROM, and the same as it in every byte the game's code was compiled
from and in the cartridge header (the generator writes those ranges beside
the ROM, in `<prefix>_rom.c`). Otherwise the game says which byte and does not
start. So an image cannot make the C and the ROM disagree about what the code
is.

What the game holds an image to and what a tree calls code are one answer
of one analysis: the tree's files of code and its header file, and nothing
in a data file. An address the analysis looked at and decoded nothing at,
because no instruction has that opcode or because the instruction would run
on into declared data, is code to neither. No C is made of it, its bytes are
data in the tree, and a game that does go there runs it in the interpreter.

What is code is what the analysis decoded, and that depends on its options.
Make the export for `split` with the options the game was generated with
(`--pointer-scan`, `--lift-edges`, and the rest), or a region the tree calls
data may be code to the game, and an edit there is refused when the game
starts.

One thing an image does not go with:

- `--data-mod` (`DATA_MODS.md`). A data-mod artifact is bound to the exact
  ROM, and refuses any other. Data mods are packages to hand to other people,
  with no byte of the ROM in them; a data tree is the ROM itself in pieces,
  for working on at home.

## Save states and the image

A save state is of the game, and the game is the ROM the program was built
from. An image is that game with other data, so a state does not belong to
the image it was made with: a state made with the ROM loads in the game
given an edited image, a state made with an image loads with another image
and with none, and a state of another game is refused (`Savestate ROM
mismatch`), as it always was. So an edit deep in the game is looked at from
a state made there, and not by playing there again from switching on:

```bash
./output/game/build/game --rom-image output/data/game.gb --load-state-file there.state
```

The state is the machine, not the ROM. What the game had read from the ROM
when the state was made is in its memory and stays there: a picture that is
on the screen, a name that was copied into RAM, a string half printed. The
edit shows when the game next reads those bytes from the ROM: when it draws
the sprite again, prints the text again, enters the room again. The game
says so in a line when it loads a state made with another image than the
one it has (in the menu, beside the slot that was loaded):

```
[GBRT] there.state was made with another ROM image of this game: what it holds in memory from that image stays until the game loads the data again
```

How a state names its game. Its header has the hash of the ROM the machine
had, as it always did: the image's, where there was one. A state made with
an edited image also has a hash of the ROM its game was built from, in four
bytes of the header that were padding before and are zero in every older
state. A game takes a state made with the ROM it has now, image or not, or
with the ROM it was built from, or one that names the ROM it was built
from. So:

- a game given no image makes the states it always made, byte for byte, and
  so does a game given an image that is the ROM;
- every state made before loads where it loaded before. One that was made
  with an edited image before this names no game, and goes with that image
  only;
- what reads a state (`gb_discover.py`, `glyphs`) reads it as before:
  nothing in the file has moved.

A recorded run (`RUN_RECORDS.md`) is of one image, on purpose: the record
has the image's hash, and a replay goes on only from a state made with that
image. `--replay-run` with `--load-state-file` refuses any other, as before.

## What the game does with its data

A region is a run of bytes between code, and nothing in the ROM says what is
in it. The game does, by what it does with the bytes:

```bash
./output/game/build/game --data-use output/data/use.json        # play, or script it with --input
python3 tools/gb_data.py uses output/data/game output/data/use.json
```

`--data-use FILE` has a generated game run in the interpreter and follow
every byte it reads from the ROM as data: through the registers, through RAM
and the stack, to where it is written or used. A byte taken off the stack
while the stack pointer is in the ROM, which is how some games copy pictures
in the little time the screen gives them, is such a read; it is put down to
the instruction that pointed the stack pointer there, since such a copy is a
row of `POP`s that each read two bytes of every so many. When the game
stops, the file
(`gbrt.data-use`, version 1) lists the bytes that were read, in runs, each
with what became of them:

| Use | The byte, or a value worked out from it, |
|---|---|
| `tiles` | was written to the tile area of video memory (8000-97FF): a picture. |
| `tile_map` | was written to a tile map (9800-9FFF): text, or a map. |
| `oam` | was written to the sprite table, by the game or by the hardware's own copy. |
| `sound` | was written to a sound register or the wave table. |
| `palette` | was written to a palette register. |
| `bank` | was written to the cartridge: a bank number, mostly. |
| `jump` | was half of an address the game jumped to: the address of code. |
| `pointer` | was half of an address the game read or wrote through: the address of data. |

`as_it_is` lists the uses of the byte itself, copied unchanged; `worked_out`
the uses of a value the game made from it (a letter with an offset added,
or a picture that was unpacked). `read_at` is the instruction that read the
byte first, and `one_reader` whether it was the only one. A byte that was
read and went to none of these was compared, counted with, or kept in RAM.

`gb_data.py uses` takes one record or several (several runs of the game add
up), or a folder, which stands for the `*.json` in it, as it does for
`annotations`: a game played from hundreds of save states writes more records
than a command line holds. It writes them into the manifest: each data region gets `seen`, the
runs of its bytes that were read, placed from the region's start. That is
what says that this stretch of a region is pictures and that one text. It
also counts the bytes the game read as data that lie in files of code: the
analysis decoded something there, and the game says it is data.

Last it lists the bytes by the instruction that read them (`readers` in the
manifest has the whole list). One instruction reads one kind of thing, and
that often says more than where each byte went: the loop that copies reads
pictures, the routine that prints reads text, the routine that unpacks
reads packed pictures, of which no byte arrives anywhere as itself.

The game plays the same while it records: the test for it ends a recorded
run and a plain one in the same state. It is slower, since nothing compiled
runs, though the interpreter is fast: Pokémon Red records 1,500 frames in
under a second.

What the record is and is not. A value is followed as one byte, and where
two bytes meet in an instruction the result carries one of them (the one
just brought to the other). So it says what a stretch of data is for, which
is what it is for; it does not prove where every bit of a byte went. A value
kept in RAM keeps its tag there, including across the stack, but RAM and
cartridge RAM are followed by address alone, not by bank. And it is a record
of a run: data the game did not use in that run is not in it.

## Where a value in RAM comes from

`--data-use` starts at the ROM and follows its bytes to where they are used.
Much of what makes a game what it is never arrives anywhere that way: how
much damage a hit does, what a thing costs, when a level is gained. Those are
numbers in RAM, worked out by code from tables, from constants written into
instructions, and from other numbers in RAM. They are found by starting at
the other end: pick the variable, and ask of every write to it which
instruction wrote, and what the value was made of.

```bash
# 1. Play to shortly before the thing happens, and keep a save state (F5 in the window, or --save-state-file).
# 2. Run on from the state, watching the variable: here two bytes, for six hundred frames.
./output/game/build/game --headless --save-dir output/scratch --load-state-file output/before.state \
    --limit-frames 600 --trace-back output/data/hp.json --trace-back-of CFE6-CFE7

# 3. Read it. Names come from a symbol file or a names.sym, functions from an export, instructions from the ROM.
python3 tools/gb_trace_back.py show output/data/hp.json --rom roms/game.gb \
    --sym output/src/game/names.sym --export output/data/game-export.json
```

`RUNTIME.md` has the flags. A watched address is any RAM from `8000` up:
work RAM, cartridge RAM, high RAM, video memory, the sprite table.

### Reading a tree

This is the whole tree of one write, from the game the test builds. The game
reads the third entry of a table in bank 3, the entry a variable picks, and
stores it:

```text
wOutTable (C010) = $30
  [1] C010 written at 00:01F5 TableEntry+11: ld [$C010], a  (1 write; frame 0)
  inside, outermost first:
    00:01EA TableEntry, called from 00:0161
  made of     ROM 03:4102 Table+2 = $30
  through     constant $4100 (Table) at 00:01F0 TableEntry+6: ld hl, $4100
  through     constant $00 at 00:01EE TableEntry+4: ld d, $00
  through     [2] wIndex (C000) = $02, written at 00:01AA Init+2: ld [$C000], a
    made of     constant $02 at 00:01A8 Init: ld a, $02
  in all, made of:
    ROM 03:4102 Table+2  1 byte
  in all, loaded through:
    constant $4100 at 00:01F0 TableEntry+6: ld hl, $4100
    constant $00 at 00:01EE TableEntry+4: ld d, $00
    constant $02 at 00:01A8 Init: ld a, $02
    RAM:        wIndex (C000)
  functions on the way: TableEntry; Init
```

The first lines are the write: the instruction, how often it wrote and when,
and the calls it was made in, each by the address the call arrived at and
where it was made from. Under them is what the value was made of, and each
line begins with how the thing took part:

| | |
|---|---|
| `made of` | It was copied or reckoned into the value: the byte that was loaded, both sides of a sum, the carry a sum took in. |
| `through` | The value was loaded through an address made of it. `ld a, [hl]` gives the byte at HL as what the value is made of, and what H and L were made of as what it was loaded through: the table's start, and the variable that picked the entry. |
| `decided by` | A conditional jump, call or return tested it, and the code that then stored the value ran because of how the test came out. It had a say in whether this was stored, not in what was. |

And each line is one of these things:

- **A numbered line** is an instruction that wrote a byte of RAM, with the
  value and how many times it wrote in this tree; what its values were made
  of is under it, further in. An instruction in a loop that stores to one
  byte sixty times is one line that says so, with the sources of all sixty
  under it and `its own earlier value` for the times it built on what it had
  stored before. An instruction that is in the tree twice is written out
  where it is nearest the watched write, and elsewhere is only its number.
  `pushed at` is a `PUSH`; a value that went over the stack has the push in
  its tree.
- **`ROM BB:AAAA`** is a byte of the ROM that was read as data, with its
  value: a table's entry. Neighbouring bytes are one line.
- **`constant`** is the operand of an instruction, with the instruction: the
  `5` of `ld a, 5`, the `100` of `cp 100`, the address of `ld hl, Table`. A
  word that is an address is given its name.
- **`hardware`** is a register that was read: the divider a random number is
  made of, the buttons.
- **`as it was when the trace began`** is a byte nothing has written since
  the trace started, or since the save state was loaded. The trace knows no
  more of it. To see behind it, start earlier: from a state before the
  battle began, if it is a stat that was set as the battle began.
- **`as something that is no instruction left it`** is a byte that does not
  hold what the last write the trace saw put there: a program playing the
  game wrote it (`--control`), or the hardware dropped the write. Anything
  else that changes RAM without an instruction shows the same way.

The summary at the end is everything in the tree by how it took part, the
nearest first. What only decided comes last and is counted off by how many
decisions lie between it and the write: `decided by`, then `decided by what
decided that (2 decisions away)`, and so on.

`--no-decisions` leaves out everything that only decided, `--decisions N`
follows at most N decisions one after another, and `--depth N` prints N
instructions deep. `--of ADDRESS[-ADDRESS]` picks writes; bytes next to one
another that were written one after the other in the same calls are shown
as one value, with the bytes in the order of their addresses. `--all`
prints every tree of a write and not only the first, `--json` writes the
same for a program, and `--tree DIR` adds the file of a data tree each ROM
byte is in. `gb_trace_back.py summary RECORD...` takes many records and
says, for each watched address, the instructions that wrote it, the ROM
that fed it and the RAM on the way.

### What a tree does not say

Believe a tree for what it shows, and do not take what it leaves out as
proof of anything. It is bounded everywhere, and it says where:

- **It is one run.** A branch the game did not take in it is not there.
- **A value keeps eight sources.** When a ninth arrives the weakest is
  dropped: a decision before an address, an address before a value. A line
  `more decisions than a value keeps: some were dropped`, or `more sources`,
  says that it happened.
- **The trace remembers so many megabytes of events** (`--trace-back-memory`),
  and a tree that leads back to an older one says `forgotten`. A tree is
  taken when the watched write is made, so what it holds is safe from then
  on.
- **A tree goes so deep and takes so many events** (`--trace-back-depth`,
  `--trace-back-nodes`); `not followed` counts what was cut.
- **Only the first write of each instruction, with each set of calls around
  it, gets a tree** (`--trace-back-each`, `--trace-back-trees`). Every write
  is counted.

Decisions are the weakest part, and are kept apart for that reason:

- **They say too much.** A decision reaches as far as the code a jump
  guards: the instructions a jump not taken runs into before its target,
  the loop a jump backwards repeats, what a taken jump lands on where
  nothing else runs on into it (the code before it ends in a return, or in a
  jump over it), the rest of a function after a conditional return that was
  not made, and all of a function called on a condition or jumped to through
  a register. Everything stored or reckoned there counts as decided, whether
  it would have come out differently or not. A loop's counter decides every
  store in the loop.
- **They say too little.** A jump that skips a store decides what the byte
  then still holds, and the trace does not see a store that was not made:
  where the taken path runs on into code the other path reaches too, nothing
  is noted. A function does not inherit what its caller decided before
  calling it. And what a tested value had itself been decided by is not
  passed on, or one test would spread to everything.
- **Arithmetic done in a loop hides its operands behind them.** A multiply
  that shifts and adds stores a sum that is made of the multiplicand, and
  only decided by the multiplier. A divide that subtracts and counts stores
  a count that is made of nothing but itself, decided by the dividend and
  the divisor. So the inputs of a formula are often not `made of` at all:
  they are several decisions away, among other things that are that far
  away for no good reason, such as the button that chose the move. Read the
  nearest decisions, and follow them an instruction at a time.

And it is a trace of bytes, by the instruction:

- Where two bytes meet in an instruction the result is made of both, and of
  the carry where the instruction takes it in. A pair that is stepped (`inc
  hl`) is still what it was made of.
- RAM is followed by the byte it really is: video memory, work RAM and
  cartridge RAM by their banks. The ROM is by its offset.
- The calls are known by the stack pointer: a call is over when the stack
  pointer has passed its return address. A function reached through another
  that only switches the bank is listed under the address the call arrived
  at, which is that one's. The calls in progress when the trace began, or
  when a state was loaded, are not known.
- A read of a hardware register is a `hardware` leaf. Where the register
  holds what was last written to it, that write is in the tree as well.
- The sprite table copied by the hardware (the write to FF46) and a block
  copied into video memory by a Game Boy Color are followed, a byte at a
  time. Code that runs from RAM is followed like any other; its constants
  are given with the address they were at.

The record is JSON (`gbrt.trace-back`, version 1): `writes` has one entry
for each instruction that wrote a watched byte with each set of calls
(`address`, `at`, `count`, `calls`, and its `trees`, each naming an event),
and `events_kept` has every event a tree reached, once, with its `sources`.
A source is a short list: its kind (`rom`, `constant`, `constant_in_ram`,
`hardware`, `ram`, `start`, `start_register`, `unseen`), its role (`value`,
`address`, `decision`), and then the ROM offset and the byte, or the
operand's offset, size and value, or the register and the instruction that
read it, or the id of another event.

## Telling the analysis what is data

What is code is what the analysis decoded, and it decodes too much: where a
stretch of data can be read as instructions and something seems to lead
there, it is taken for code. Such bytes are in files of code. No picture can
be drawn of them and no image may change them, and a sprite or a tileset
with a stretch like that in it lies in pieces.

The game knows better, and a record of it played says so. `gb_data.py
annotations` writes what the game showed to be data as a file for the
analysis:

```bash
# 1. From a first export, tree and game: play with --data-use, as above.
# 2. What the game showed to be data.
python3 tools/gb_data.py annotations output/data/game output/data/game.use.json -o output/data/game.annotations

# 3. The export and the game again, told. The same options to both, as always.
./build/bin/gbrecomp game.gb --pointer-scan strict --annotations output/data/game.annotations \
    --lift-export output/lift/game.json
./build/bin/gbrecomp game.gb --pointer-scan strict --annotations output/data/game.annotations -o output/game

# 4. A tree of the new export, with what the old tree holds, and the same records into it.
python3 tools/gb_data.py split output/lift/game.json --rom game.gb -o output/data/game2 --keep output/data/game
python3 tools/gb_data.py uses output/data/game2 output/data/game.use.json
```

**What counts.** A byte the game used as it is: drew (the tile area, a tile
map, the sprite table, a palette), played (a sound register), sent to the
cartridge as a bank number, or took for half of an address it jumped to or
read through. A byte that was only read, or that something was only worked
out from, is not shown to be anything, since a game reads its own code too:
to copy a routine into RAM, or to add itself up. So nothing that ran is
called data on the strength of having been read.

**The file** has a line for each run of such bytes, in the form `gbrecomp
--annotations` reads, with what the bytes were used for as a comment:

```
data 05:4180 384 ; tiles
data 00:01AE 12 ; pointer
```

It holds every such byte, whether the analysis had it as data or not, so it
is a fact about the ROM and the records and not about one analysis: made
again from a tree that was split with it, it is the same file. The tool says
how many of the bytes are in files of code now. `gbrecomp` takes one
annotations file; lines of one's own (`function`, `label`, more `data`) can
go in the same file after it is written.

**What the analysis does with it.** It decodes no instruction that starts
in a declared range, as before, and now none that would run on into one:
the bytes in front of a table often spell the start of a longer instruction,
which used to take the table's first bytes for its operand and leave them
in a file of code. If an annotation is ever wrong, and the game does run
those bytes, they run in the interpreter: slower, and the same.

**Splitting again.** The new export cuts the ROM into other regions, so its
tree goes into a folder of its own, and `--keep` takes over what was done in
the old one: edits, pictures, text, what the game unpacks (*A tree split
again*, below). `split --force` writes a tree afresh where one is, and
refuses one that has edits. A game made without the annotations refuses an
image that changes bytes it has as code, and one made with them takes it.

What this does not reach: data the game was not seen to use in any record,
and data it never uses as it is. A packed picture is the second kind, since
every byte of it is unpacked into something else.

## Finding the routine that unpacks

A packed picture is never used as it is. The game reads it a byte at a time
through one instruction and makes something else of every byte, so a record
says only that the bytes were read, and by which instruction. To edit such
a picture without writing a packer for the game, the game's own routine is
left to unpack it, and what it made is replaced as the routine returns
(*Packed data, edited as what the game unpacks*, below). That takes knowing
which routine it is, and this step finds it.

```bash
# 1. The instruction that reads the packed bytes: `uses` lists the instructions that read,
#    and none of this one's bytes arrives anywhere as itself.
python3 tools/gb_data.py uses output/data/game output/data/use.json

# 2. Play again, following it.
./output/game/build/game --data-use output/data/follow.json --data-use-follow 00:01B2

# 3. Which function unpacks, how it is told where its stream is, and what it makes:
#    written as the declaration the tree keeps.
python3 tools/gb_data.py unpackers output/data/game output/data/follow.json -o output/data/game/unpackers.json
```

`--data-use-follow BANK:ADDRESS` (hexadecimal, up to four of them) has the
recorder keep a shadow of the call stack while it records. A call begins at
a `CALL`, an `RST` or an interrupt, and is over when the stack pointer
passes the place its return address is at. So a function that ends by
jumping to another is still the same call, and one that throws its return
address away is over. The first instruction of a function that something
has called is an entry however the game comes to it: by a jump, or by
running on into it from the code before.

For each call in progress while the followed instruction read, the record
(`followed`, in the same file) has where the call was entered and from
where, the registers and the mapped ROM bank as it was entered, the runs of
ROM bytes the instruction read while the call lasted, and the runs of RAM
that instructions stored to while it lasted: video memory, cartridge RAM,
work RAM and high RAM, without what the stack wrote and without what an
interrupt's routine stored in between. It also has which words of RAM held
the address of a run's first byte when the call was entered. The calls are
counted for the function they are calls of, and up to sixteen of each
function's, one for each stream, are kept whole as samples.

A call that read is one of these:

| | The call |
|---|---|
| `whole` | read one run of bytes from its first, and nothing read on from the run's end afterwards. |
| `joined` | read one run, which went on from where an earlier call stopped, or which a later call went on from. |
| `several` | read more than one run. |
| `left` | did not come back to where it was called from. |
| `undecided` | read one run from its first byte, and the record ended before the instruction read anywhere else. |
| `unfinished` | was still going when the record ended. |

The function the instruction is in reads a byte a call, each joined to the
last. The function that unpacks reads a run whole every call. So do the
functions that call it once each, and further out a function reads several
runs. `gb_data.py unpackers` adds the counts up over the records, prints
the functions nearest first, and proposes the nearest that reads whole: the
tightest routine around one stream. From that function's samples it works
out:

- **where the stream is**: a register pair that held the address of the
  run's first byte in every sample, or failing that a word of RAM that did;
- **its bank**: a register that held the bank number in every sample, or
  failing that the bank that was mapped as the function was entered;
- **what it made**: the runs of RAM that are at the same address with the
  same size in every sample. A run only some calls write is left out, and
  so is one of fewer than 16 bytes or less than an eighth of the largest:
  those are counters and flags the routine keeps for itself. The report
  lists what was left out, and why.

If one of the three cannot be worked out for the nearest function, the tool
says which and tries the next further out. `-o FILE` writes the proposal as
a declaration (`gbrecomp.unpackers`, version 1):

```json
{"schema": "gbrecomp.unpackers", "version": 1, "unpackers": [
 {"name": "unpacker_00_01B9",
  "function": {"bank": 0, "address": 441},
  "stream": {"bank": {"register": "a"}, "address": {"register": "hl"}},
  "made": [{"address": 49408, "size": 32}],
  "why": "…"}
]}
```

A value is a number, a register (`{"register": "hl"}`), a byte or a word of
RAM (`{"ram8": 65424}`, `{"ram16": 49632}`), or the mapped bank
(`{"mapped_bank": true}`), each as the function is entered. Where the
routine fills a buffer its caller names, the address of a `made` run is the
register pair that pointed at it.

It is a proposal, made from what the records show. Read the report before
using it:

- **It is a record of runs.** One stream is no proof of a register, and one
  bank no proof of where the bank is; the report says how many it saw.
  Record more, from more places in the game: several records add up.
- **A function nothing calls is not seen.** A routine that is only ever
  jumped to has no entry the recorder knows, and the function that jumps to
  it stands for it. The report says how the proposed function was entered;
  a relative jump, or running on into it, is neither a call nor a `JP`.
- **Streams that lie one after the other in the ROM, and are unpacked one
  after the other, read as one run.** Both calls then count as `joined`. A
  function is still taken to read whole with one call in ten joined.
- **Output is told from scratch by size alone.** A routine whose output is
  of a different size for every stream gets no proposal, since a
  declaration has one size, and output smaller than 16 bytes is left out
  with the counters.
- **Not everything is followed.** Code that runs in RAM is not, and a call
  made more than 256 deep is not, which the record counts. A game that
  points the stack pointer at something in RAM to copy with looks as if its
  calls had ended.
- **It finds the routine and no more.** How to draw what the routine makes
  (`as`) is for whoever reads the proposal to add, and a run that is two
  bit planes one after the other is to be written as two pieces for that.

## Tiles as pictures

Graphics that are not compressed are tiles of 8 by 8 pixels: 16 bytes each
at 2 bits a pixel, or 8 bytes at 1 bit a pixel, as many fonts are.
`gb_data.py tiles` draws a run of them as a PNG beside the file they are in.
From then on the picture can be edited in place of the bytes.

```bash
# 96 tiles of a tileset that starts 0x3E4 bytes into its file, 16 to a row.
python3 tools/gb_data.py tiles output/data/game bank_1A/49DB_unknown.bin --offset 0x3E4 --tiles 96

# 24 tiles of a sprite, two to a row.
python3 tools/gb_data.py tiles output/data/game bank_05/4146_unknown.bin --offset 0x3A --tiles 24 --width 2

# 19 tiles of a font at 1 bit a pixel.
python3 tools/gb_data.py tiles output/data/game bank_04/575F_unknown.bin --offset 0x321 --tiles 19 --bpp 1

# Edit the PNGs, then build and play as before.
python3 tools/gb_data.py status output/data/game
python3 tools/gb_data.py build output/data/game -o output/data/game.gb
```

With a file named, the tool does not find tiles (with none named it does:
*Tiles found from a played game*, below). It is told where they are: the file, how far
into it they start (`--offset`, 0 unless given), how many there are
(`--tiles`), and how many bits a pixel (`--bpp 2` or `--bpp 1`). Without
`--tiles` it draws the rest of the file, and stops if that is not a whole
number of tiles. Only a `data` file can be drawn.

Where the tiles are has to come from somewhere else. A disassembly's symbol
file says it. So does what the game was seen to do (`seen`, above): a
stretch whose bytes went to the tile area as they are is tiles, and `tiles`
with no file named draws those. Failing both, draw a file and look.

The picture is `<file>.tiles.png`, or `<file>.tiles_AAAA.png` when it does
not start where the file does, `AAAA` being the address of its first tile.
It is an 8-bit greyscale PNG with `--width` tiles to a row: 16 unless given,
or all of them where there are fewer (tiles a record shows on the screen are
drawn in the game's colours instead: the next section). Colour 0 is white, colour 1 light grey
(170), colour 2 dark grey (85) and colour 3 black; at 1 bit a pixel, 0 is
white and 1 is black. These are the numbers in the tile, not the colours on
the screen, which the game's palettes choose. Where the last row is short of
tiles the rest of it is white, and belongs to no tile.

The manifest records the picture on its region, under `views`:

```json
"views": [
 {"kind": "tiles", "file": "bank_05/4146_unknown.tiles_4180.png", "offset": 58, "tiles": 24,
  "width": 2, "bpp": 2, "sha256": "…", "data_sha256": "…"}
]
```

`sha256` is of the PNG as `tiles` wrote it, and `data_sha256` of the bytes
it was drawn from. A file can have several pictures, each of bytes of its
own; `tiles` refuses bytes that are already drawn. `kind` says what the view is: `tiles`, or `text` (below). `build` stops at
any other.

`build` reads the pictures. One that is as `tiles` wrote it changes nothing:
the file stands, edited or not, so with nothing edited the image is still
the original ROM. One that has been edited is read back into tile bytes,
and in the image they take the place of those bytes of the file. The file
itself is not written to. If the file was edited in the same bytes after
the picture was drawn, and the two do not say the same thing, `build` stops
and names both; `tiles --force` draws the picture again from the file.
`tiles` without `--force` does not draw over an edited picture. A picture
the manifest names has to be there: `tiles` draws a missing one again.
`status` lists edited pictures with edited files.

A picture keeps its size in pixels. Otherwise any PNG an ordinary editor
saves is read: greyscale, with or without alpha, RGB, RGBA, or indexed, at 8
bits a sample, and greyscale or indexed at 1, 2 or 4 bits a pixel. Each
pixel is taken as the colour whose grey it is nearest to in brightness, so
a picture need not hold exactly the four greys. Alpha is ignored: a pixel
rubbed out to transparent is whatever colour is under the transparency,
which is often black. Interlaced PNGs and 16 bits a sample are refused.

Drawing and reading back loses nothing. Every colour of every pixel has a
grey of its own, so any bytes drawn as tiles and read back are the same
bytes, tiles or not.

## Tiles in the game's colours

A tile is colour numbers, 0 to 3. What colour a number is, the game's
palettes say, and they are set while it runs: nothing in the ROM says which
palette goes with which tiles. A played game does.

A game run with `--data-use` looks at its screen once a frame, when the
frame is whole. For every tile that drew a pixel it notes which layer drew
it, the background (the window counts as that) or a sprite, and the palette
it was drawn through: on a Game Boy the value of the palette register at
that pixel, on a Game Boy Color the palette's four colours as they are when
the frame ends. Where the bytes of such a tile lie in video memory as the ROM
has them, the record says so, under `shown`:

```json
"shown": {"frames": 900, "dropped": false, "tiles": [
  {"offset": 82304, "size": 64, "as": [{"by": "objects", "palette": 208, "frames": 394},
                                       {"by": "objects", "palette": 0, "frames": 44}]},
  {"offset": 4416, "size": 16, "planes": 3,
   "as": [{"by": "background", "colours": [32767, 22197, 10570, 0], "frames": 20}]}
]}
```

A run is ROM bytes that were shown alike: by which layer (`background` or
`objects`), through which palette (`palette`, the register's value, or
`colours`, four colours of 15 bits), and in how many frames a tile they are
in drew at least one pixel so. `planes` is there where every byte of the run
was the same half of its pixels: 1 the low bits, 2 the high bits, 3 both,
which is how a font of one bit a pixel is written out. `dropped` is true if
the recorder ran out of memory and left something out. The rest of the
record is what it was, a record without `shown` is read as before, and the
game ends as it does unrecorded.

`uses` adds the records up and writes `shown` on each data region, the
palette seen longest first. `tiles` then draws any tiles the screen showed
as an indexed PNG:

- **its palette** is the one those tiles were seen through: first the layer
  that drew them longest, a frame counting for each byte it showed, and of
  that layer's palettes the one seen longest among those that keep the most
  colour numbers apart. A screen that is all white while the game loads, or
  half way through fading, draws several numbers alike, and is passed over
  however long it lasted, unless nothing else was seen;
- **a pixel's value is its colour number**, at 2 bits a pixel, so nothing is
  lost where two numbers have the same colour. A font of one bit a pixel has
  two colours, 0 and the number its bytes make where they are set;
- **colour 0 is clear** in a picture of tiles that sprites drew, since a
  sprite does not draw its colour 0;
- `--greys` draws the plain greys instead, and tiles that no record shows on
  the screen are drawn in them as before.

`tiles` says which palette it took. The view keeps it, as `palette`: red,
green, blue and how solid, two hexadecimal digits each, for each colour
number (`["ffffff00", "ffffffff", "aaaaaaff", "000000ff"]`). A Game Boy's
four shades are drawn as the four greys.

**Reading such a picture back.** `build` and `status` read it by the same
rules as a grey one: as `tiles` wrote it, it changes nothing; saved again
and showing the same tiles, it changes nothing; edited, its tiles take the
place of the bytes. What a pixel says depends on how the picture was saved:

- still indexed, with the palette it was written with: the pixel's value is
  its colour number, whatever colours share a shade;
- otherwise by colour: a clear pixel (less than half solid) is colour 0
  where colour 0 is clear, and any other pixel is the colour number whose
  colour it is nearest to;
- where two colour numbers have that colour and the picture does not tell
  them apart, the pixel keeps the number the tile has there now, if that is
  one of the two. If it is not, nothing says which is meant, and `build`
  stops and names the pixel. It does not guess.

So a sprite whose colours 0 and 1 are both white is read right from an RGBA
save, where the clear white is 0 and the solid white is 1. Saved as RGB,
which has nothing clear, a white pixel stays what the tile has if that is 0
or 1: a white painted over a darker colour is refused, and a solid white
painted over the clear one cannot be told from it and changes nothing. The
same holds for a PNG that calls one colour clear wherever it is, as some
programs save an indexed picture turned to RGB: the solid white is clear
with it. Keep what is clear when saving a sprite, or draw it with `--greys`.

What this does not do:

- **One palette a picture.** A sheet whose tiles were shown through
  different palettes is drawn in one of them. Draw the tiles that belong
  together as a picture of their own.
- **Only tiles the recorder followed.** A record started from a save state
  knows nothing of what was in video memory already, and bytes the game
  copies with the stack pointer in the ROM (`pop`) are not followed by the
  recorder at all. Tiles that are unpacked are worked out, not copied:
  `unpacked` pictures are grey.
- **The frame's end.** A tile whose bytes the game changes while the frame
  is drawn is credited with what is in video memory when it ends, and so is
  a Game Boy Color palette. A game for the Game Boy run on a Game Boy Color
  is recorded by its palette registers.
- **Sprites are not put together.** A picture is tiles in the order they lie
  in the ROM. Which tiles make a sprite, and where, is in the object table,
  which a game works out and does not copy.

## Tiles found from a played game

Nobody has to say where the tiles are if the game was seen to draw them.
`tiles` with no file named makes a picture of every run of tiles a recorded
game copied to video memory as they are:

```bash
# 1. Play with the recorder noting where tiles land as well.
./output/game/build/game --data-use output/data/use.json --data-use-tiles

# 2. Into the tree, as before.
python3 tools/gb_data.py uses output/data/game output/data/use.json

# 3. A picture of every run of tiles the game drew, a sheet of them all, and a list.
python3 tools/gb_data.py tiles output/data/game

# 4. Look through output/data/game/tiles.png, edit the pictures, then build and play as before.
```

**What the record has to say.** That a byte went to the tile area says it
is part of a picture. It does not say which byte is the first of a tile, or
whether a tile is 16 bytes or 8. A font kept at 1 bit a pixel is copied by a
loop that writes every byte twice, and in a record its bytes are tiles like
any others.

`--data-use-tiles`, with `--data-use`, adds that. The recorder notes the
address each byte landed at when it reached the tile area as it is, however
the game read it (with `POP` too: *What the game does with its data*,
above). The record gets a section, `tile_runs`:

```json
"tile_runs": [
 {"offset": 82304, "size": 192, "bank": 5, "address": 16768, "step": 1, "to": 32768, "begun": "told"},
 {"offset": 82496, "size": 192, "bank": 5, "address": 16960, "step": 1, "to": 34816, "begun": "worked_out"},
 {"offset": 72320, "size": 1024, "bank": 4, "address": 23168, "step": 2, "to": 34816, "begun": "unknown"}
]
```

A run is a byte a copy began with and the bytes that followed on from it:

- `to` is where its first byte landed. The low four bits of an address in
  the tile area are the place in a tile, so the byte that lands on a
  multiple of 16 is the first of one.
- `step` is how far each byte landed after the one before it in the ROM: 1
  for tiles at 2 bits a pixel, 2 for tiles at 1 bit a pixel, where the game
  writes each byte twice or to every other address. A byte that landed
  alone has 0.
- `begun` is how the copy came by the address it read the first byte at:
  `told`, if the address is a word of the ROM as it stands, as in a table of
  sheets; `worked_out`, if the game made it from something in the ROM, as
  the second half of a sheet is read at the address of the first and 192;
  `unknown` otherwise, which is a constant of the code, or a number that
  was in RAM before the record began.
- a run with `mixed` in place of those landed at more than one place in a
  tile. Such bytes are a table the game looks things up in, not tiles.

The section is all the option adds: the rest of the record is exactly what
it is without the option, and the game ends in the same state either way.
`uses` adds the records up and
writes each region's runs into the manifest (`tile_runs`, placed from the
region's start). Records made without the option add no runs; they are
taken as before. Where records disagree, a byte that landed at two places in
a tile is `mixed`, and `told` goes before `worked_out`, and that before
`unknown`.

**Which bytes are a picture.** `tiles DIR` goes by these rules:

- A run is a picture, from its first whole tile to its last. Bytes at
  either end that are not a whole tile are left out.
- A run that begins where the one before it ends goes on with that picture
  if its `begun` is `worked_out` and its first byte is where the picture
  would have put it. So a sheet copied in two halves to two places is one
  picture, and the sheet after it, which the game was told the address of,
  is another. A byte a copy was told to begin with begins a picture even
  where another copy ran on through it.
- A run stops at the first byte the game also used as it is for something
  else. A game may copy more than a picture's worth: one that copies 96
  tiles for every tileset copies the start of what lies after a short one.
- Bytes between two runs that no record shows in any use are left out, and
  counted. They might be tiles the game did not get to. In the game this was
  measured on (below) every such stretch was another sheet nobody had met,
  and drawn with its neighbours it would have made one picture of three
  things.
- A picture is 16 tiles to a row, or all of them where there are fewer
  (`--width` for another number). The record does not say how a game puts a
  picture's tiles together on the screen.

**What is written.** Each picture is a view like one told by hand, in the
same file names, with `"from": "seen"` in the manifest to say nobody told
`tiles` of it. `build`, `status` and editing are as for any picture. The
command prints what it drew and what it left out and why, and writes two
things to look through at the top of the tree: `tiles.png`, a sheet of every
picture of tiles in the tree under the place of its first tile
(`BANK:ADDRESS`), and `tiles.txt`, a line for each with its file.

It can be run again at any time, after more records and `uses`:

- run again with nothing new, it changes nothing;
- a picture somebody told `tiles` of stays as it is, and so do the strings
  of a file of text: a picture that would take any of their bytes is not
  drawn, and the report says which;
- a picture it made before is drawn again, with more tiles if the records
  now show more, and taken away if the runs no longer make it;
- a picture that has been edited is not written over or taken away, and
  keeps its bytes. `--force` draws everything afresh, and the edits are
  lost;
- `tiles DIR FILE` over the bytes of a picture found this way takes its
  place, unless that picture has been edited.

**Tiles in files of code** cannot be pictures, since code cannot be edited.
The command counts the bytes of such runs and lists where they are.
`annotations` writes them down for `gbrecomp` (*Telling the analysis what
is data*, above), and a tree split from an export made that way has them in
data files.

What this does not find:

- **What no record shows.** A sheet the game did not load while it was
  recorded has no picture, and one it loaded part of has that part.
- **One sheet read at two addresses the code spells out.** Where the game
  reads the second half of a sheet at an address written in its code, or at
  the first with a constant added, nothing says the two belong together,
  and they are two pictures. Where a table has an address for a part of a
  sheet, the part is a picture of its own.
- **The end of a picture, where the game copies more.** What is copied after
  the last tile is drawn with it, unless the game was seen to use those
  bytes for something else.
- **Tiles kept some other way.** Bit planes kept apart, tiles turned about,
  and anything the game works out are not followed as tiles. Packed
  pictures are the next section but one.
- **How a picture is put together.** A sprite of four tiles is four tiles
  in a row here. That takes following tile numbers through the tile maps
  and the sprite table, and is not built.
- Copies the hardware makes into video memory on a Game Boy Color are fed
  to the same code, and have not been tried on a game.

## Text as text

Nothing in a ROM says which bytes are text, or what letter a byte is. What
the game was seen to do says both, so this step starts from a record
(`uses`, above) and a save state.

```bash
# 1. Play with --data-use, and write what was seen into the tree.
./output/game/build/game --data-use output/data/game.use.json --save-state-file output/data/game.state
python3 tools/gb_data.py uses output/data/game output/data/game.use.json

# 2. The game's letters, as a picture, and a charmap to fill in from it.
python3 tools/gb_data.py glyphs output/data/game output/data/game.state

# 3. With the charmap written: the strings of each region, as a file of text beside it.
python3 tools/gb_data.py text output/data/game

# 4. Edit the .text.txt files, then build and play as before.
python3 tools/gb_data.py status output/data/game
python3 tools/gb_data.py build output/data/game -o output/data/game.gb
```

**Which bytes are text.** A game prints through one routine, and one
instruction of it reads every string, a byte after a byte; nearly all the
bytes it reads arrive in a tile map as they are. So an instruction that read
at least 16 bytes, three quarters of which reached a tile map as they were,
and that read them in runs (two or more on average), is taken to read text,
and each run of bytes it read is a string, with the bytes that break a line
or end the string in it, since the same instruction reads those. The runs
are what tells printing from drawing a map out of blocks of tiles, whose
bytes all reach the tile map too: there each of several instructions reads
every few bytes. `text` says which instructions it took. `--reader
BANK:ADDRESS`, repeatable, names them instead; `uses` lists the candidates.

**What letter a byte is.** A byte that reaches a tile map draws the tile of
that number, and the tiles are in video memory while text is on the screen.
`glyphs DIR STATE` draws, out of a save state made at such a moment, the
tile each of the 256 bytes draws: `glyphs.png`, sixteen to a row, the byte's
first digit down the side and its second along the top, with the bytes the
strings use dark and the rest faint. It also writes `charmap.json` at the
top of the tree, listing those bytes with no letters, unless one is there.
Reading the sheet is the one step a person or a model does for a game, once:

```json
{"schema": "gbrecomp.charmap", "version": 1,
 "letters": {"7F": " ", "80": "A", "81": "B", "4F": "{line}", "50": "{end}", "E1": "{PK}"}}
```

What a byte is written as is up to whoever writes the charmap, and can be
more than one character: a name in braces serves for a tile of two letters,
and for a byte that draws nothing and tells the routine to break the line or
stop. `<`, `>` and the end of a line cannot be used. A byte left empty, or
left out, is written `<XX>` in the text, its number in hexadecimal, and can
be typed that way too. Give each byte a spelling of its own: where two
bytes, or two runs of bytes, are spelt alike, an edited string is written
with the longest spelling at each place and the lowest byte that has it.

**The files of text.** `text DIR` writes, for each data region with strings
in it, `<file>.text.txt` beside the region's file, and records it under
`views` with the strings it holds:

```
+0F28 48 |{PLAYER} is{line}playing the SNES!{cont}...Okay!{cont}It's time to go!{done}|
```

A line is a string: where it starts in the region's file (hexadecimal), how
many bytes it has, and between the bars what it says. Edit between the bars
and leave the rest. `build` and `status` read the files as they read
pictures:

- a file as `text` wrote it changes nothing, and neither does a line that
  still says what its bytes say, so text that was only saved again, with
  other line ends or after the charmap learnt a letter, is not an edit, and
  an unedited tree is still the original ROM;
- an edited string is written over the old one in the image, and the
  region's file is not written to;
- **a string cannot grow where it lies.** The bytes after it belong to
  something else. A longer string is moved, with what it is part of, where
  the game was recorded reading it through pointers that lie in data (*Data
  that grows, by moving*, below). Otherwise `build` stops, and says how many
  bytes there is room for and why the string cannot move;
- a shorter string is written over the start of the old one, and the rest
  of the old one stays where it is. That is right when the new string ends
  itself as the old one did, and `build` says so when it does not. A string
  cut short and not ended would change nothing, and is refused;
- if the region's file was edited inside the strings after `text` wrote the
  text, and the text says something else, `build` stops and names both;
  `text --force` writes the text again from the file. Without `--force`,
  `text` does not write over text that has been edited: it stops, and writes
  nothing. `text --keep-edited` leaves each file of text that has been edited
  as it is, with the strings it had, writes the others, and says how many it
  left, as `maps` does by itself.

A region can have pictures and text, of different bytes: `tiles` refuses
bytes that have strings in them, and `text` leaves out a string that lies in
a picture.

**The strings nobody saw printed.** A game prints what somebody plays as far
as, and most of its text is said by people nobody talked to. `text --read`
finds those too, by reading:

```bash
python3 tools/gb_data.py text output/data/game --read
```

The strings that were seen printed say what a string is made of. A byte
that ends two of them or more, and ends one more often than it stands inside
one, ends strings: the last letter of a name that fills its room is the last
byte of a string too, and ends nothing. Every byte the charmap has a letter
for can stand inside one. So the data is gone through for stretches of such
bytes with such an end, and a stretch is taken for a string where at least
half of it is letters and spaces and four of them are letters, which chance
seldom gives. One that has less is taken where it lies within four bytes of
a string, seen or read: the short names of a list lie one after another, and
a sentence with a name put into it goes on after the name with as little as
a full stop.

Three kinds of bytes are not looked at. Bytes a string or another view
already has. Bytes the game was seen to use for something that is not text:
drawn as a tile, played, sent as a bank number, jumped through, or with
something worked out from them. (Bytes that reached a tile map as they are
may be text all the same: a name copied into memory and printed from there
is read by a copying loop.) And packed data, which is letters by chance
more often than anything else: everything from where a stream the game
unpacks starts (`unpacked`, below) to the next stream, or to the next view
or string after the last.

A string that was read is a string like the others: a line in the file of
text, edited and built the same way. What it is not is proven. The view in
the manifest says which of its strings they are (`read`), and `text` says
how many. One that is no text does no harm while its line is left alone:
the tree still builds to the original ROM. A string the game is later seen
to print is from then on a string that was seen. `split --keep` carries
them as strings and does not carry which they were; `text --read` on the
new tree says again.

It needs strings to learn from, so `uses` and a charmap come first, as
above. A charmap that has only the letters the seen strings use finds only
the strings made of those: the more of the sheet somebody has read, the more
is found.

What this does not find:

- **Strings unlike any that was printed.** Without `--read` a string is
  known only by having been read: more play recorded, and `uses` and `text`
  again, finds more, and a string that was only half printed when a record
  ended is there by half. With it, a string is still missed that has a byte
  the charmap has no letter for, that ends with a byte no seen string ends
  with, or that is short, not beside another, and less than half letters.
- **Where a string that was read is used.** No record saw the game read it,
  so nothing says what leads to it, and it cannot be made longer by moving
  (*Data that grows, by moving*) until a record does.
- **Text that is copied before it is printed**, where `--read` is not asked
  for. The record keeps the first instruction that read a byte. Names a game
  copies into memory and prints from there are read by its copying loop,
  which reads pictures too, so the rule for readers does not find them and
  `--reader` cannot single them out. `--read` finds them as it finds the
  others.
- **Tile maps that are not text.** A screen laid out by a loop that reads
  its tile numbers in a run looks the same as a string to the rule. Its
  bytes are then in a file of text, as `<XX>` or as whatever letters those
  tiles are; nothing is lost by it.

## Tile maps as tile numbers

A byte that reaches a tile map as it is and is not text is a tile number:
of a screen the game copies whole, or of one of the blocks of tiles it
builds its maps from. Nothing in the ROM says how wide such a screen or
block is, or which tiles its numbers mean. Where the bytes lay on the screen
says both.

```bash
# 1. Play with --data-use, and write what was seen into the tree, as for text.
python3 tools/gb_data.py uses output/data/game output/data/game.use.json

# 2. The tile numbers of each region as a file beside it, and a picture of them.
python3 tools/gb_data.py maps output/data/game

# 3. Edit the .map.txt files, build and play as before. `maps` again draws the pictures again.
python3 tools/gb_data.py build output/data/game -o output/data/game.gb
```

**What the record says.** When a frame is whole, the recorder works out
which cells of the two tile maps were on the screen, from each pixel the
background or the window drew and from the scroll and window registers as
they are then. A cell is kept only if its tile number gives the tile that
pixel was drawn from, so a game that scrolls in the middle of a frame is not
mistaken. For the ROM byte in each such cell, where it lies there as the ROM
has it, `shown` has a line under `maps`:

```json
{"offset": 1589, "cells": 38, "left": 38, "after": 0, "above": 38, "under": [[5, 38]],
 "tiles": [[16656, 16, 38]]}
```

`cells` is the cells it was in, added up over the frames. `left` is how many
of those had the cell to their left on the screen too, and `after` how many
of those held the byte before this one in the ROM: the two are then in one
row. `above` is how many had the cell above on the screen, and `under` how
many bytes before this one that cell held, how often: that is how many tiles
a row is. `tiles` is the ROM tile the number drew: where its first byte is,
how many bytes it is (16, or 8 where each byte was written to both halves of
its pixels), and how often. These count cells and not frames, since a block
of tiles is on the screen many times at once; and a neighbour is counted
against the cells where it could be seen at all, since at the edge of the
screen it is out of sight, which says nothing.

**Rows and grids.** `uses` adds the records up and writes `mapped` on each
data region: rows of its bytes, each with the tile every byte drew most. A
byte goes on the row of the byte before it where it lay right of it in more
than half of the cells where that could be seen, and a row is `under` so
many bytes where more than half of the cells above its tiles say so. `maps`
puts rows together into grids: rows that follow one another in the file,
each as wide as the one before and under exactly that many bytes. A screen
of twenty tiles by eighteen is one grid. A set of blocks is a grid for each
block, unless the game always puts two of them one under the other.

**Text is left alone.** Bytes that the instructions that read text read are
no tile numbers: the instructions `text` finds, or the ones named with
`--reader BANK:ADDRESS`, as for `text`. Nor are bytes a picture or a file of
text has. `--reader none` takes every byte that was in a tile map for a
tile number, which is for a game whose screens are copied by a loop that the
rule takes for a routine that prints.

**The file** is `<file>.map.txt`, beside the region's file:

```
+05E0 4x4
0B 0B 0B 0B
0B 0C 0D 0B
0B 1C 1D 0B
0B 0B 0B 0B
```

A grid starts with where it is in the region's file (hexadecimal) and how
many tiles it is across and down, and has a row of tile numbers to a line,
in hexadecimal. Edit the numbers and leave the rest. The manifest records
the file under `views`, with `kind` `map` and its `grids`, each an offset
and a size across and down. `build` and `status` read it as they read a
picture or a file of text:

- a file as `maps` wrote it changes nothing, and neither does one that still
  says the numbers it was written from, however it was saved again;
- an edited number takes the place of its byte in the image, and the
  region's file is not written to;
- a grid keeps its place and its size: a row too short, a grid that is gone
  or of another shape, and something that is no number are refused by name;
- if the region's file was edited in those bytes as well and says something
  else, `build` stops and names both.

`maps` does not write over numbers somebody edited: it leaves that file and
its view as they are and says so. `--force` writes them again from the
region's file.

**The picture**, `<file>.map.png`, is for looking at, and is not read back.
Every time `maps` runs it is drawn again from the tree as it is now: the
grids side by side, the tile numbers as the files and the edited views have
them, each drawn as the ROM tile that number stood for, out of the tree's
bytes for that tile, in the palette the tile was seen through (*Tiles in the
game's colours*). So an edited number and an edited tile both show in it. A
number that was changed, or whose own tile no record has, draws what the
nearest byte of the region with that number was seen to draw, since a game's
tile numbers mean the same tiles from one block of a set to the next. A
number nothing is known of is hatched, and so is one whose tile did not come
from the ROM as it is: an unpacked one, or one the game draws itself.

What this does not do:

- **Only what was on the screen.** Bytes that reached a tile map and were in
  no cell of the screen when a frame was whole have no rows; `maps` counts
  them. A block that was only ever at the edge of the screen is there in
  pieces, the rows of the part that showed.
- **A record started from a save state** knows the tiles that were copied
  since. A number that was only on the screen before that has no tile of its
  own, and is drawn as the nearest that has.
- **Rows as the ROM has them.** A screen kept a column at a time, packed, or
  put together by code is not found, and a map that names blocks (a byte for
  a block of sixteen tiles) is not a tile map: its bytes reach no tile map
  as they are.
- **The window** is taken to have been on from its first line down. Where a
  game turns it off and on inside a frame, its cells fail the check above
  and are left out.
- **Game Boy Color attributes.** The bytes of the second bank's map, which
  give a cell its palette, its bank and its flips, are not a view. The
  picture draws a tile from the bank the game drew it from, unflipped, in
  the palette that tile was seen through most and not the cell's own.
- **Sprites are not laid out.** Which tiles make a sprite, and where, is in
  the object table, which a game works out and does not copy from its ROM.

## Packed data, edited as what the game unpacks

A packed picture cannot be edited where it lies. The bytes in the ROM are
the packer's, and writing new ones takes the packer, which no game ships and
which is different for every game. What every game does ship is the routine
that unpacks, and it makes the same bytes of each packed stream every time,
somewhere in RAM. Those bytes are the picture.

So the routine is left to run, to its end, every time, and what it made is
what gets edited: the game is told to put the edited bytes in place of the
ones the routine made, as the routine returns. No packer is written, the
ROM image is not touched, and the routine takes the time it always took.

```bash
# 1. Say which routine unpacks: unpackers.json, at the top of the tree (below).
# 2. Build. Beside the image there is now a pack, game.gbup, which so far only says what to watch.
python3 tools/gb_data.py build output/data/game -o output/data/game.gb

# 3. Play with the pack. The game writes out what the routine made of each stream it was given.
./output/game/build/game --unpacked output/data/game.gbup --unpacked-harvest output/data/game.harvest.json

# 4. Into the tree, as pictures: output/data/game/unpacked/<name>/BB_AAAA.png
python3 tools/gb_data.py unpacked output/data/game output/data/game.harvest.json

# 5. Edit the pictures, build, and play with the pack.
python3 tools/gb_data.py status output/data/game
python3 tools/gb_data.py build output/data/game -o output/data/game.gb
./output/game/build/game --unpacked output/data/game.gbup
```

**The declaration**, `unpackers.json`, is a few lines a game, written by
whoever has found the routine:

```json
{"schema": "gbrecomp.unpackers", "version": 1, "unpackers": [
  {"name": "pictures",
   "function": {"bank": 0, "address": 9469},
   "stream": {"bank": {"register": "a"}, "address": {"ram16": 53419}},
   "made": [{"address": 41352, "size": 392}, {"address": 41744, "size": 392}],
   "as": {"kind": "planes", "order": "columns",
          "tiles_wide": {"stream": 0, "shift": 4, "mask": 15},
          "tiles_high": {"stream": 0, "mask": 15}},
   "why": "what was seen that says so"}
]}
```

`function` is where the routine is entered. `stream` is where its packed
bytes start, and `made` the pieces of RAM its output lies in when it
returns. Where a number is wanted for the stream's bank or address, or for
the address of a piece, it is a number, or how to find it at the moment the
routine is entered: `{"register": "a"}` (`a b c d e h l bc de hl`),
`{"ram8": ADDRESS}`, `{"ram16": ADDRESS}`, or `{"mapped_bank": true}` for
the ROM bank mapped at 4000-7FFF. A piece is a size in bytes at an address
in video memory, cartridge RAM, work RAM or high RAM.

`as` says how to draw what was made. Without it the bytes are edited as
bytes, in a `.bin`. Two ways of drawing are known:

- `{"kind": "tiles", "bpp": 2, "width": 16}`: ordinary tiles one after
  another, as `tiles` draws them;
- `{"kind": "planes", "order": "columns" or "rows", "tiles_wide": N,
  "tiles_high": N}`: each piece of `made` is one bit of every pixel, the
  first piece the low bit, a byte for eight pixels across, and the bytes run
  down the picture a column of eight pixels at a time, or across it a row
  at a time. A number of tiles can be a byte of the packed stream itself,
  `{"stream": OFFSET, "shift": N, "mask": M}`, for a stream that starts with
  its picture's size.

**The harvest.** A game given a pack and `--unpacked-harvest FILE` plays as
it always does and, each time a declared routine returns, keeps what it
made, once for each different thing made of each stream. The file is
written when the game stops. Harvest as much play as there is: `unpacked`
takes any number of harvests, or a folder of them, adds what is new, and
leaves what is in the tree alone, edits included (`--force` writes
everything again). Only what
was unpacked while somebody watched is in the tree (*Packed data nobody
played as far as*, below, has the game unpack the rest). A harvest made with an
edited `--rom-image` is refused, since what the routine makes of edited
data is not the game's own.

**In the tree** each thing made has two files under `unpacked/<name>/`,
named for where its stream is: `BB_AAAA.made`, the bytes the routine made,
which are not to be edited, and `BB_AAAA.png` (or `.bin`), which is. The
manifest lists them under `unpacked`.

**One stream unpacked two ways.** A routine can make different things of
the same stream: a game that wants a picture mirrored may unpack it a second
way. Each different thing made has files of its own (`BB_AAAA_2`). Where two
of them are each other with the bits of every byte moved about the same way,
which is what a mirrored form is, `unpacked` says so and they are kept in
step: edit either picture, and `build` works the other out from it. One of
the two looks like the picture and the other does not; edit the one that
does.

Where every stream that was harvested both ways shows the same move of
bits, at least two of them, and the move undoes itself, `unpacked` writes
the other way for the streams it has only one way of, worked out from the
one it has (`worked_out` in the manifest). So a picture that was only ever
harvested mirrored still has a form to edit that looks like it. What was
worked out is in no harvest, and it is a guess that costs nothing: its
replacement is used only if the game makes exactly those bytes. Once a
harvest has it, it is no longer a guess.

**The pack**, `IMAGE.gbup`, is what `build` writes beside the image whenever
the tree has an `unpackers.json`: the declaration in the form the game
reads, and for each edited picture the bytes to have instead, with the hash
of what the routine makes. The game puts them there as the routine returns,
and only where what the routine made has that hash. So a replacement can
never land on something it was not drawn from: a stream the image changed,
or a way of unpacking nobody harvested, is left as the routine made it.
`--report-unpacked` says how often each routine was entered, returned and
had its output replaced, and how often it made something else of a stream
that has a replacement. A pack is of one ROM, the one the game was built
from, and the game stops at a pack of another. It holds no byte of the ROM.

What stays the same: the image. `unpacked` things are not in it, so `build`
makes the original ROM whatever pictures are edited, and the pack goes with
any `--rom-image` and with save states. A game given a pack that only
watches ends in the same state as one given none; with replacements, the
registers, the counters and the time are the same and only the bytes
replaced, and what the game makes of them, differ.

That difference is why a recorded run (`--record-run`, `RUN_RECORDS.md`)
keeps what was replaced. A run played while a pack replaces something has
in its record how many replacements the pack had and a hash of them, and
replays only with a pack that has the same ones; a run played with nothing
replaced is not replayed with a pack that replaces something. Either is
told before the replay starts. A pack that only watches is no difference,
so a recorded run can be replayed and swept with `--unpacked-harvest` to
have everything the game unpacked while it was played. For a run played
with nothing replaced that takes a pack with no replacements in it.

So where the game has to be the game as it is, the pack has to be one that
only watches. `build` writes that pack only while nothing unpacked is
edited. `watch-pack` writes it from the tree as it stands, edited or not:

```bash
python3 tools/gb_data.py watch-pack output/data/game -o output/data/game.watch.gbup
```

It is the tree's declaration with no replacement in it, byte for byte the
pack `build` wrote before the first picture was edited, and the tool says
that it replaces nothing. It reads no picture, so an edit that `build`
would refuse is not in its way, and it leaves the pack beside the image
alone. Two things want it. A recorded run that was played with nothing
replaced is replayed and swept with it and `--unpacked-harvest`: with a
pack that replaces something the replay is refused before it starts. And
`--data-use` takes no other pack.

What this does not do:

- **The routine has to be the right one.** *Finding the routine that
  unpacks*, above, proposes the declaration from a recording; it is a
  proposal, and what the pictures look like says whether it is right.
- **It replaces at the return.** A routine that unpacks straight into video
  memory while the screen is on shows its own picture until it returns.
- **Same size, same place.** A replacement is as many bytes as the routine
  makes, where the routine makes them.
- **It sees a routine entered by a call, a restart or a `jp`,** from
  recompiled, interpreted or lifted code. One that is fallen into, or
  reached by a `jr`, is not seen, and nor is anything in a project generated
  without cycle counting. `--report-unpacked` shows a routine that is never
  entered.
- **A state saved half way through a routine** and loaded again finishes
  that one unpacking unwatched.
- `--differential` does not take a pack, and `--data-use` takes one only if
  it has no replacements: a record is of the game as it is. `watch-pack`
  writes such a pack from any tree.

## Data that grows, by moving

A file keeps its size and its place, so a string made longer has nowhere to
go where it lies: the bytes after it are something else. It can go
somewhere else, if everything that reads it is told. What reads it has its
address from a pointer, and a pointer is one of two things: two bytes in a
table, which are data, or the operand of an instruction (`ld hl, $4D7D`),
which is code. Data can be changed in an image. Code cannot: the game's C
was compiled from it, and the game refuses an image that differs there. So
what can move is what the game reaches only through pointers that lie in
data: entries of pointer tables, the pointers inside scripts and records.

```bash
# 1. Play with the pointers recorded as well, and write what was seen into the tree.
./output/game/build/game --data-use output/data/game.use.json --data-use-pointers
python3 tools/gb_data.py uses output/data/game output/data/game.use.json
python3 tools/gb_data.py pointers output/data/game output/data/game.use.json

# 2. Room: the empty ends of banks, proposed. Read the proposal, then keep it in the tree.
python3 tools/gb_data.py free output/data/game -o output/data/game/free.json

# 3. Make a string longer in its .text.txt, and build and play as before.
python3 tools/gb_data.py status output/data/game
python3 tools/gb_data.py build output/data/game -o output/data/game.gb
./output/game/build/game --rom-image output/data/game.gb
```

**The record.** `--data-use-pointers`, with `--data-use`, has the recorder
note which two bytes of the ROM made each address the game read its ROM
through. A value is followed as "this byte of the ROM, give or take a plain
number": through registers, RAM and the stack, through `inc` and `dec`, and
through a constant or a number out of RAM being added, which is what
stepping along and indexing do. The two bytes of an instruction's operand
(`ld bc`, `ld de`, `ld hl`, `ld a, [nn]`) are followed the same way, and
marked as an operand. Anything else done to
the value ends it, and so does adding two bytes of the ROM together, since
nothing says which of them was the address. The record gets a `pointers`
section, and is otherwise the file it was:

- `links`: each pair of bytes that made addresses (`low`, `high`, and
  whether it is an `operand`), the `bank` of what was read through it, the
  `first` and `last` offset read, and the `least` and `most` that had been
  added to it;
- `reached`: for every byte that was read, whether it was reached through
  a pair read as `data`, through an `operand`, or through an address that
  was neither (`other`), which is also what the hardware's own copies are;
- `numbers`: the bytes that were numbers to the game. A byte that was
  compared, masked, shifted, had a register taken from it, or was lost in a
  sum, was reckoned with, and an address that is reckoned with cannot be
  changed without changing what comes of the reckoning. An operand is
  marked for the same, and for one thing more: the game jumped to what it
  had become (`jp hl`, or a `ret` to it), which is how a string with code
  behind it is read and then run on from. Bytes read as data that are
  jumped through are known by their use, as they always were.

**The things.** `pointers DIR RECORD...` cuts each bank where its files
start and where pointers lead. A thing is the bytes from where one pointer
leads to where the next one does, or to the end of its file. Where the game
read on through one pointer past the place another leads to, the two are
one thing with two pointers, one of them into its middle. The manifest gets
`things`: for each its place and size, the runs of its bytes that were
read, the pointers that lead into it, the parts of it that lie in files of
code (`pinned`), and whether it `moves`, with `why` not. It moves if every
read of it in the records came through pointers each of which

- was read as data, and lies in a data file;
- is used for nothing else: not jumped through, not drawn, not a number;
- shares no byte with another pointer, and was read with one bank mapped;
- still says what it said when the game was recorded.

The report counts the rest by reason. The largest count is what an
instruction's operand leads to: that stays until the instruction is lifted
C that names the data ([Data the source names](#data-the-source-names)).

**Free space.** Nothing is written anywhere that is not declared free, in
`free.json` at the top of the tree (`gbrecomp.free-space`, version 1: runs
of a `bank`, an `address` and a `size`). `free DIR` proposes the end of
each bank: the run of one byte value up to the bank's last byte that is
data, that no record read and nothing was drawn from, less its first 16
bytes, since what lies in front of it may end in the same value. It is a
proposal: a game may keep something in bytes nobody has seen it read.
`build` checks a declaration byte by byte, and refuses one that names code,
bytes a record read, a pointer, what a pointer leads to, or bytes a picture
or a file of text has.

**Growing.** A string made longer in its `.text.txt` is no longer refused.
`build` finds what was changed in the line, moves the thing that is part
of into the free space of its bank, with the new string in it and what
followed the string following it, and turns the thing's pointers to the
copy. A pointer that lies inside a thing that moves is copied turned. The
report says what moved, where to, and what each pointer said and says.
`status` says the same without writing. The old bytes stay where they
were, and the tree's files are not written to: a build is still only an
image. What cannot be moved is refused with the reason: an instruction's
operand leads to it, no record says what leads to it, no free space is
declared, or there is no room in its bank.

**The proof.** `build --move-all` moves every thing that can move, as far
as there is room, the smallest first, and turns each byte the game was
seen to read at an old place into its opposite. Whatever still read one of
those would read something else. A game played from that image by the same
buttons, showing the same frames and ending in the same state but for the
bytes of RAM that hold an address, read all of it at the new places. The
bytes of a thing that lie in files of code are left as they are, since no
image may change them. Such an image is for proving the pointers and not
for playing: what no record saw read is still at the old places, and those
are spoilt.

What this is and is not:

- **It is what the records show.** Moving is a copy, so a pointer nobody
  recorded goes on leading to the old place, and shows what was there
  before. A thing ends where the next recorded pointer leads, so with few
  records a thing is large: everything up to the end of its file. Record
  more, and the things get smaller.
- **A thing stays in its bank.** The two bytes of an address are turned,
  and not the byte that says the bank. A bank without free space, as a bank
  full of music is, has nowhere to move anything to.
- **What follows a string in its thing moves along with it.** That is
  right where the game comes to it by reading on, and wrong where it
  counts from the thing's start. Bytes put in exactly where a pointer
  leads are the start of what it leads to.
- **A thing the game reads past the end of, in a way no record shows,**
  reads what lies after its copy.
- **Reckoning the recorder does not see.** It marks a pointer the game
  compares or does arithmetic on. It does not see a pointer that only
  decides which way the game goes by counting down, or by a carry out of a
  sum. The proof is what finds such a thing: compare the state at the end,
  not only the frames.
- **Code that runs in RAM** is seen an instruction at a time, the stubs
  the runtime has for it included. A read through the stack pointer, which
  a game that copies with `pop` makes, is not recorded at all, as it is not
  by `--data-use`.

## Data the source names

An instruction's operand is code, and no image may change it. The game's
source can: lifted source kept in a folder (`LIFTING.md`, *Data the source
names*) writes `ld hl, $43DE` as `ctx->hl = BaseStats;` for every operand a
record saw the game read its ROM through, and a name is whatever the
compiler is told it is. What such an operand leads to can then move like
anything a pointer in data leads to. This is the data tools' half of it.

```bash
# The source folder, told what the tree's records saw, and built.
python3 tools/gb_lift.py source output/lift/game.json -o output/game-src --data-tree output/data/game
python3 tools/gb_lift.py build output/game-src

# The tree, told which operands the game has by name.
python3 tools/gb_data.py pointers output/data/game output/data/records \
    --named output/game-src/.lift/named-operands.json

# An image, with what grew moved (everything that can, with --move-all), and the header that
# says where to; the game again, for that image.
python3 tools/gb_data.py build output/data/game -o output/data/game.gb \
    --moved-header output/game-src/src/lifted_moved.h
python3 tools/gb_lift.py build output/game-src
./output/game-src/build/x64-release/game/game --rom-image output/data/game.gb
```

`pointers` takes a folder of records as well as files, since a command line
does not hold hundreds of names.

**`pointers --named FILE`.** The file is the source folder's
`.lift/named-operands.json`: the operands the game has by name, how far each
name runs, and every other operand that spells an address in the ROM. A
named operand is judged as a pointer in data is, except that it need not
lie in a data file. In the manifest its pointer has `named`, the name, and
`runs`, the size. A thing stays, with the reason in `why`, where

- an operand that leads to it is not named;
- the game compared a named operand, reckoned with it, or jumped to what it
  made (`numbers` in the record);
- the game read the operand's own bytes as data: code it copies and runs
  somewhere else has the ROM's address in the copy;
- an operand that is not named spells an address one of its pointers leads
  to (`spelled`), whatever the records saw of that operand.

**`build --moved-header FILE`.** A named operand is not turned in the image:
its bytes stay as they are, and the image is still one the game takes.
`build` writes where each name leads now into FILE, as `#define`s, with the
image's SHA-1 as `LIFTED_MOVED_IMAGE`. FILE is the source folder's
`src/lifted_moved.h`, and the game has to be built again from it. Without
the option `build` refuses to move a thing the source names. With it the
file is written by every build, empty of names when nothing the source names
moved, so that a header left from another image cannot stay behind.

A thing a named operand leads to is copied as far as `runs` says, where
that is more than the records read, and not past the end of its bank.

**Beside the image**, `<image>.moved.json` (`gbrecomp.data-moved`, version
1) lists what a build moved: `moved` (the `offset`, `size`, where `to`, and
the `new_size`), `turned` (each pointer in data, what it `said` and `says`)
and `named` (each name likewise). It is for whatever has to tell an address
in the image from the same address in the ROM, as a comparison of two games'
memory has to. A build that moves nothing removes it.

What this is and is not:

- **The game and the image are a pair.** A game built with a header reads
  the places of that header's image. Given the ROM, or another image, it
  reads something else there and does not notice.
- **`--move-all` is still the proof**, and now of the names too: the game
  built with the header, played from the moved image by the buttons of the
  recorded runs, has to end as the original does on the ROM.
- **A name is right for the instructions a record saw.** The same address in
  an instruction nobody saw run is a number in the source, and keeps what it
  spells where it is. An address such an instruction only comes near, by
  adding to it, is not seen at all.

## Packed data nobody played as far as

A harvest holds what the game unpacked while somebody played, so a tree has
the pictures that were met. The routine that unpacks is there for all of
them: entered with another stream's bank and address, it unpacks that one.
`gb_data.py unpack` has the game do that, once for each stream it is told
of, with nobody playing, and writes a harvest for each.

```bash
# The streams of a list, each unpacked in a save state of the game.
python3 tools/gb_data.py unpack output/data/game --game output/game/build/game \
    --state output/data/game.state --streams streams.txt -o output/data/called

# Into the tree, like any harvest.
python3 tools/gb_data.py unpacked output/data/game output/data/called
```

**The call is made by the game.** A game given `--unpacked PACK` and
`--unpacked-call BANK:ADDRESS` loads its save state if it is given one, and
then, where it would have gone on playing, enters that function: a return
address is pushed, a halted machine is woken, and the function runs in the
machine as the state left it. After the address, each behind a comma, comes
what to set first, in hexadecimal and in the order given: a register
(`a=0D`, `de=4000`), a byte at an address (`D0AA=01`: RAM, or below 8000 a
write to the cartridge, which is how a bank is mapped), and `ime=1` or
`ime=0` for interrupts on or off.

The game stops as the first routine of the pack returns, when what it made
has been kept for the harvest. So nothing depends on where the function
would have gone back to, and nothing needs to be known of the save state's
file. It also stops, with exit code 1 and a line that says which, when the
function comes back and no routine has returned, and when none has returned
after `--unpacked-call-cycles N` cycles (41,943,040 unless given, ten seconds
of the game's time): a game with its screen off never ends a frame, so
frames are not what is counted. The function's bank has to be the one
mapped. A game not given the option does none of this.

`unpack` starts the game once for each stream, several at a time
(`--workers`, 4 unless given and at most 8), so a stream that goes wrong
loses only itself. Without `--state` the function is entered in the game as
it is switched on, which is enough for a routine that sets up all it uses.
The game is run without its sound hardware (`--no-audio`), as it always was,
unless the state was made by a game that had it: a state loads only in a
game run as the one that made it was, and the state's header says which.

**What a game needs is declared** with its unpacker, in `unpackers.json`.
With nothing more than was there, the routine itself is entered, with the
stream's bank and address put where its `stream` says they are found: in a
register, or a byte or a word of RAM. `call` says otherwise, each part of
it only where the game needs it:

```json
"call": {"function": {"bank": 0, "address": 14059},
         "stream": {"bank": {"register": "a"}, "address": {"register": "de"}},
         "set": [{"ram8": 53418, "value": 0}],
         "variants": [{"name": "mirrored", "set": [{"ram8": 53418, "value": 1}]}],
         "interrupts": false,
         "cycles": 8000000}
```

- `function`: another function to enter, one that leads to the routine.
- `stream`: where that function takes the stream's bank and its address.
- `set`: what else to set first, in order: `{"register": "b"}`, `{"ram8":
  ADDRESS}`, `{"ram16": ADDRESS}` or `{"cartridge": ADDRESS}`, each with a
  `"value"`: a number, or `{"stream": "bank"}` or `{"stream": "address"}`
  for the stream's own. A routine that reads its stream from the bank that
  is mapped (`{"mapped_bank": true}`) needs the write that maps one:
  `{"cartridge": 8192, "value": {"stream": "bank"}}`.
- `variants`: the stream unpacked once more for each, with more set. A
  routine that makes a mirrored picture when a byte of RAM says so has that
  byte here, and the mirrored form is then harvested, not worked out.
- `interrupts`: on or off while the routine runs. As the state has them,
  unless said.
- `cycles`: how long to wait for the routine.

A declaration without `call` is what it was, and the pack `build` writes
has neither `call` nor `tables` in it.

**Where the streams are**, three ways, and any of them together:

- `--streams FILE`, repeatable: a list written by hand, a `BANK:ADDRESS` in
  hexadecimal to a line. What follows a `#` or a `;` is a comment.
- `--tables`: the tables of pointers the unpacker declares beside `call`,

  ```json
  "tables": [{"bank": 14, "address": 22804, "entries": 47, "entry_size": 5,
              "pointer": 0, "stream_bank": 19}]
  ```

  `entry_size` is 2 unless given, and `pointer` is where in an entry the
  address lies, two bytes with the low one first, 0 unless given.
  `stream_bank` is the bank the streams are in: a number, or `{"entry":
  OFFSET}` for a byte of the entry. Left out, it is the table's own bank.
  An address below 4000 is in bank 0 whatever is said, and an entry that
  points at nothing in the ROM is counted and left out.
- `--walk`: walking on from the streams that are known, which are the
  tree's own (what play showed) with the list's and the tables'.

**Walking on.** Packed streams mostly lie one after another, so where one
ends the next probably starts. Where one ends is known once the game has
unpacked it with a record kept (`--data-use`): the stream is the run of
bytes read from its first byte on. So each known stream is unpacked with a
record, the byte after its last is given to the game as a stream, and so on
from each one that holds.

What this finds is a guess, and a routine that unpacks makes something of
nearly any bytes. A guess is dropped, and its chain ends there, when

- the routine does not return in the cycles it is given, or does not read
  the guess's first byte, or reads on out of the bank;
- it reads on past the start of a stream that is known;
- what it made cannot be drawn the way the declaration says: a size no
  picture has, where the size is a byte of the stream;
- the guess starts with 16 bytes that are all the same, or nothing but one
  value is left to the end of the bank: padding;
- a record of play in the tree (`uses`) has one of its bytes read first by
  an instruction that reads no known stream: it is something else's data.

A chain that arrives exactly at the start of another known stream is borne
out: the guesses in it tile the bytes between two streams the game is known
to unpack. A chain that ends any other way is not, and it ends in the open:
its last guesses may be the last real streams of a run, or what the routine
made of whatever lies after them.

**What is written.** In the folder given with `-o`:

| | |
|---|---|
| `BB_AAAA.json` | The harvest of a stream that was known, or guessed and borne out. `unpacked` takes the folder. |
| `BB_AAAA.NAME.json` | The same stream unpacked the way the variant `NAME` says. |
| `guessed/` | The harvests of guesses that were not borne out, each with a picture, `BB_AAAA.png`, where the declaration says how to draw one. Look before taking them: `unpacked DIR OUT/guessed`. |
| `run/report.json` | Every stream (`gbrecomp.unpack-report`): how it was found (`list`, `table`, `tree`, `walk`), its harvests, how many bytes it read, for a guess the stream it follows (`after`), the known stream its chain arrives at (`meets`) and how many of its bytes lie in files of code (`in_code`); and under `ends`, where each chain went no further and why. |

The summary says the same in numbers, and names the guesses that lie wholly
in files of code: where the analysis found instructions a stream seldom lies
whole, though part of one often does. The exit code is 1 if a stream that
was known, or a variant of any stream, could not be unpacked, and those are
listed. `--walk-limit N`
stops a walk after N guesses (4,096 unless given), and `--keep-records`
keeps the records under `run/records`.

What this does not do:

- **It finds no stream that nothing leads to.** Walking goes forwards. A
  stream that lies before the first known one of its run, or in a bank
  where none is known, takes a list or a table. Walking backwards is not
  built: a start that was only guessed is not shown right by its end, since
  a routine that reads a byte at a time falls into step with the stream it
  started inside.
- **A routine that reads ahead** of what it uses ends its run past its
  stream, and the next guess starts late. So does a game that pads between
  its streams. Both end the chain, and neither is tried around.
- **A borne out guess is evidence, not proof,** and an open one is less. A
  guess that is not a picture is found by looking at it.
- **A table has its streams' bank in it, or one bank for all.** A bank the
  game works out some other way cannot be declared.
- **A stream below 4000 read by a routine that takes the mapped bank** is
  harvested under that bank, and `unpack` then says the routine was given
  another stream.
- **The state has to be one the routine can run in.** What the routine
  needs besides its stream (a buffer's address in RAM, the cartridge's RAM
  switched on) is as the state has it, or is declared in `set`.
- `--unpacked-call` is not taken with `--benchmark` or `--differential`, and
  a run is not recorded or replayed with it.

## Names for the files

A file of the tree is called by where its bytes lie in the ROM:
`bank_0B/4D7D_unknown.bin`, `unpacked/pictures/0B_4D7D.png`. Whoever wants
one picture out of several hundred has to open them to find it. `names.sym`,
at the top of the tree, says what things are called, and `gb_data.py names`
calls the files by it.

```bash
# The names a symbol file has for what the tree has, into names.sym, and the files renamed.
python3 tools/gb_data.py names output/data/game --sym game.sym

# Write, change or delete lines of output/data/game/names.sym, then have the files follow.
python3 tools/gb_data.py names output/data/game

# Everything the tree has, with its address, its name and its file, to search in.
python3 tools/gb_data.py names output/data/game --list output/data/game.names.txt
```

**The names file** is an RGBDS symbol file, a name to a line, and anybody
can write it:

```
05:4180 RedSprite
0B:4D7D PikachuPicFront ; from game.sym
```

A line names whatever the tree has at that address: the data region that
starts there, the picture of tiles that starts there, the string that starts
there, the things a routine unpacked from the stream there. The first line
for an address is the one that counts. A line for RAM, a constant, or an
address the tree has nothing at stays where it is and names nothing yet; a
line that cannot be read is said and stops nothing. `names` with no
`names.sym` there writes one with nothing in it but how to fill it in.

**What a symbol file brings in.** `--sym FILE`, repeatable, takes the
symbol file of a disassembly, or anybody's list of labels in that form, and
gives a line, marked `; from FILE`, to each label that lies in a data region
or on an unpacked stream. In a data region means at its start or anywhere
further in: a picture or a string may start there, now or once somebody
draws it. Labels inside code are left out, since no file starts there.
Where the symbol file has several labels for one address they all get a
line: a label of its own before one inside another (`Routine.loop`), and
last a label that says where another ends (`FontEnd`, where there is a
`Font`). Delete or move a line to have another of them name the place.

Bringing in only ever adds lines. An address that has a line keeps it,
whoever wrote it, so a name somebody gave or changed stays through every
later `--sym`, which is wanted again whenever the tree has more in it. With
a line deleted, the symbol file's name is back the next time. `names` says
how many lines say something else than the symbol file does.

**What a file is called.** By its name, in the folder it was in:

| Without a name | With one |
|---|---|
| `bank_05/4180_unknown.bin` | `bank_05/RedSprite.bin` |
| `bank_05/4180_unknown.tiles.png` | `bank_05/RedSprite.tiles.png` |
| `bank_05/4180_unknown.text.txt` | `bank_05/RedSprite.text.txt` |
| `bank_05/4146_unknown.tiles_4180.png` | `bank_05/RedSprite.tiles.png` |
| `bank_05/4180_unknown.bin`, with two more names in it | `bank_05/RedSprite_and_2_more.bin` |
| `unpacked/pictures/0B_4D7D.png` | `unpacked/pictures/PikachuPicFront.png` |
| `unpacked/pictures/0B_4D7D_2.made` | `unpacked/pictures/PikachuPicFront_2.made` |

A region is a run of bytes between code, and may hold more than the thing
its first byte is the start of. Where other places in it have a name as
well, its file is called by the first and by how many others there are, as
in the fifth row. Called by one name alone it would pass for that one
thing; this way it says what it is, and still sorts and is found by a real
name. The number is of what the manifest lists for the region (`labels`,
below), less the first, and a place counts once however many labels it
has. A label that says where another ends (`FontEnd`) counts where it has
its place to itself, since something lies after that end; where it shares
its place with the name of what starts there, that one is the name. So the
file is renamed when a name is added to its region or taken out of it. A
region with names further in and none on its first byte keeps its address.

A view is one thing. It goes by the name of where it starts if that has
one, as in the fourth row, at its region's first byte too. A view with no
name of its own goes by its region's file, count and all
(`RedSprite_and_2_more.tiles_4300.png`), and so does a file of text, which
has the strings of the whole region. The ways a stream was unpacked share
the stream's name. A file of code, or the header, keeps the name it has:
that it is code, and not to be edited, is what that name is there to say.

A name is always a file name of its own. What a file system does not take
is written `_`. Two things that would get the same file name, the same
label twice or two that differ only in capitals, both keep their old name
after the label (`Label.4180_unknown.bin`), and so does a label that is a
device's name on Windows, is longer than 80 characters, or looks like one
of the tool's own names.

**Nothing in a file changes.** A file is renamed, edit and all. The tree
builds to the same image and the same pack named and unnamed, and unedited
it is still the original ROM. Naming twice does nothing the second time.
With a line taken away the file has its old name back, and with all of them
taken away the tree is the tree it was. Files are renamed all or none; a
renaming that is cut short is taken back by the next command that reads the
tree. Where a file the manifest does not know is in the way, and is not the
same bytes, nothing is renamed onto it: the thing goes without its name and
`names` says which file to move away.

**The address is not lost.** The manifest has it for every file as before,
and it follows the names: `file` is what the file is called now, and
`names` at the top has what each renamed file was called (`was`), which is
the name it gets back. A region is not cut up where a name is, so each
region lists every name that lies in it, with how far in:

```json
"labels": [[58, "RedSprite"], [442, "BlueSprite"], [826, "OakSprite"]]
```

That is where to draw: `tiles FILE --offset 58` makes a picture that is
called `RedSprite.tiles.png` at once. `--list FILE` writes the same out for
a person, a line for each region, view, string, name and unpacked thing.

**A string's name** is over its line in the file of text, since a string
has no file of its own:

```
# 22:4F28 _RedBedroomSNESText+1
+0F28 48 |{PLAYER} is{line}playing the SNES!{cont}...Okay!{cont}It's time to go!{done}|
```

`Label` alone is a string that starts at a name, and `Label+N` one that
starts N bytes after the nearest name before it in its region, which is
what a symbol file gives where its label is on a byte in front of the
letters. Those comments are the tool's, and the only thing it changes in a
file: an edited line stays as it is, and text nobody edited is still text
nobody edited.

**Afterwards** every command keeps the names. A picture drawn again, text
written again and a harvest put in again are the same files; a picture or
an unpacked thing made later takes the name that is waiting for it, without
`names` being run; and a file can still be asked for by its address
(`tiles DIR bank_05/4180_unknown.bin`). A tree split again with `--force`
has its names again, from the `names.sym` that is still there. And a tree
that was named when a file with several names in it still went by the
first alone is renamed to this the next time `names`, or any command that
writes the manifest, is run on it.

**Without a symbol file** there is still what the game was seen to do
(`uses`). `names DIR --seen` calls a data file that has no name by the use
most of its bytes were put to as they are: `4146_tiles.bin` for
`4146_unknown.bin`, and likewise `tile_map`, `oam`, `sound`, `palette`,
`bank`, `jump` and `pointer`. The address stays in front, so such a name is
not taken for one somebody gave, and a name somebody gives takes its place.
It holds for the commands that follow, `uses` with more records among
them, until `--no-seen`.

What this does not do:

- **It works no name out.** A name is in a symbol file, or somebody writes
  it. Nothing looks at a picture and says what it is of.
- **A region is not cut up.** It is a run of bytes between code and may
  hold a dozen sprites; its file says that it does (`_and_N_more`), and
  `labels` and `--list` say which, but it is one file still. A region whose
  first byte has no name keeps its address, whatever lies further in.
- **Only data is named.** Functions, RAM and constants are not brought in.
  Lines for them can be written into `names.sym` and are kept, and nothing
  uses them yet.
- **Bringing in never changes a line.** Where a newer symbol file calls
  something differently, the old line has to be deleted first.
- **A file left behind under a name** by `split --force`, with an edit in
  it, is in the way of the new file's name until it is moved.
- **Text that was edited keeps the hash of how it was last written,**
  comments included. Nothing goes by that hash once the file is edited; it
  is only that a manifest is not the same to the byte after names were
  given and taken away again around such an edit.

## A tree split again

Splitting again is ordinary work. Each time the analysis is told more of
what is data, its export cuts the ROM into other regions: files of code
become data, and two data files with a stretch of code between them become
one. By then the old tree has work in it. `--keep` takes it over:

```bash
python3 tools/gb_data.py split output/lift/game2.json --rom game.gb -o output/data/game2 --keep output/data/game
```

The new tree is written to a folder of its own, and the old one is only
read. What is kept is kept by the bytes of the ROM it is of, wherever the
new export has put them:

| In the old tree | In the new one |
|---|---|
| An edited byte | Edited, in whichever data file has it now. The image `build` makes is the one it made. |
| A picture of tiles | The same PNG, to the byte, beside the file that has all of its tiles now, named after that file and with its new offset. |
| A file of text | Its strings, each in the text of the file it lies in now: one file's strings may go to several, and several files' to one. An edited string says what it was made to say, and comment lines (`#`) stay with their strings. |
| What the game unpacks, `unpackers.json` | Copied: none of it depends on where the regions are cut. The pack `build` writes is the one it wrote. |
| `charmap.json`, `glyphs.png` | Copied. |
| What `uses` wrote | Carried where every byte of data in the new tree was data in the old one: `seen` is cut to the new regions and `readers` added up again, and both are what `uses` would write from the same records. Where the new tree has data that the old one had as code, which is what telling the analysis more does, the old manifest says nothing of those bytes: nothing is carried, and `uses` is run again with the records. |

The command ends by putting the new tree together and comparing: it has to
make the image and the replacements the old tree makes, and says so.

**What cannot go with its bytes** is left behind and named, with the reason:

- a picture whose tiles are in two files now, or in a file of code, and a
  string likewise. What was changed through it is not lost: it is in the
  bytes of the new files, and `tiles` draws the picture again from them;
- text whose charmap cannot be read, and text that would share a file with
  text of another charmap;
- a file in the old tree's folders that its manifest does not name. One at
  the top of the tree is copied as it is and named.

**An edit to bytes that are code in the new tree** stops the command, which
names the bytes and writes nothing: an image cannot change code. That
happens when the new export was made with less told than the old one. Tell
the analysis those bytes are data and make the export again, or give
`--lose-edits-in-code` to go on without them.

**The old tree has to build.** A picture and a file that disagree, or an
edited file of code, stop `--keep` as they stop `build`.

**What this tool does not know.** Something else may have put more into a
tree: a view of another kind, a field on a region or in the manifest, a name
of its own for a file. Each stays with its region where the region is the
same bytes in the new tree, and is named as left behind where it is not.

`split --force`, which writes a tree afresh where one is, does not write
over a tree that has edits: it says what is edited and stops.
`--discard-edits` writes over them. Over a tree with no edits it goes on,
and says what else the tree held (pictures, text, what `uses` wrote) that
the new one does not.

What this does not do:

- **Cut a picture in two.** A picture whose tiles the new export has put in
  two files is not made into two pictures.
- **Keep part of what `uses` wrote.** It is all or nothing: a manifest
  cannot say which of its bytes nobody has looked at.
- **Work in place.** The new tree is always another folder.

## Music and sound as commands

Music in a ROM is not notes. It is a list of commands for the game's own
sound engine: play this pitch for so long, change octave, set the tempo,
call that passage, loop, end. Sound effects are the same lists, often with
a few commands that mean something else. In the tree those bytes are bytes.
This step writes them as the commands they are, a line each, and `build`
puts an edited line back.

```bash
# 1. Say what the engine's commands are and where its lists start: streams.json, at the top of the tree (below).
# 2. The commands of each region, as a file of text beside it.
python3 tools/gb_data.py streams output/data/game

# 3. Edit the .stream.txt files, then build and play as before.
python3 tools/gb_data.py status output/data/game
python3 tools/gb_data.py build output/data/game -o output/data/game.gb
./output/game/build/game --rom-image output/data/game.gb
```

**The declaration**, `streams.json`, is what somebody writes once for a
game. Nothing in it is about one game's tunes: it says how the engine reads.

```json
{"schema": "gbrecomp.streams", "version": 1,
 "grammars": {
  "music": {"commands": [
   {"name": "note", "first": "00-BF", "operands": [
     {"name": "pitch", "bits": [4, 7], "names": ["C_", "C#", "D_"]},
     {"name": "length", "bits": [0, 3], "add": 1}]},
   {"name": "octave", "first": "E0-E7", "operands": [{"name": "octave", "bits": [0, 2], "from": 8}]},
   {"name": "tempo", "first": "ED", "operands": [{"name": "tempo", "byte": 1, "word": "big"}]},
   {"name": "call", "first": "FD", "operands": [{"name": "to", "byte": 1, "word": "little", "target": "call"}]},
   {"name": "loop", "first": "FE", "ends": {"times": 0}, "operands": [
     {"name": "times", "byte": 1}, {"name": "to", "byte": 2, "word": "little", "target": "loop"}]},
   {"name": "end", "first": "FF", "ends": true}]},
  "drums": {"like": "music", "commands": [
   {"name": "drum", "first": "B0-BF", "operands": [
     {"name": "which", "byte": 1}, {"name": "length", "bits": [0, 3], "add": 1}]}]}},
 "starts": [
  {"table": {"bank": 2, "address": "4003", "entries": 40, "entry_size": 3, "pointer": 1},
   "grammar": {"byte": 0, "mask": "0F", "is": {"0": "music", "3": "drums"}}},
  {"at": {"bank": 2, "address": "5B00"}, "grammar": "music"}],
 "readers": [{"bank": 2, "address": "5832"}]}
```

A number is a number, or a string of hexadecimal digits.

A **grammar** is a list of commands. The first byte of a command says which
it is: `first` is that byte or a range of them, and the commands are tried in
the order they are written. An engine usually needs more than one grammar,
since the channel a list plays on changes what some bytes mean: `like` takes
another grammar's commands, and a command named again takes the place of the
one it is named after.

An **operand** is some bits of one byte of the command (`byte`, 0 for the
first, and `bits`, the lowest and the highest) or two bytes (`word`, `little`
or `big` end first). The text has it as a number: with `add` added, or taken
away `from` a number, or in hexadecimal (`hex`), or as one of `names` (a
list, the name of 0 first, or an object from number to name). In the text a
number can be written `12`, `$0C` or `0x0C` whichever way it was written
out. Every bit of a command has to be said by an operand or be the same for
all its first bytes, and no two operands may say the same bit. That is
checked when the declaration is read, and it is why text written from bytes
goes back into exactly those bytes.

A **target** is an operand that is the address of more of the list, in the
bank the command is in or below 4000. `call` and `loop` are followed, and
the list goes on after the command; `jump` is followed, and the list ends
there. `ends` says a command ends its list: always (`true`), or where an
operand has a value, which is how a loop that never stops is told from one
that goes round three times. `then` names the grammar the rest of the list
is in, for an engine with a command that changes how it reads.

The **starts** are where lists begin: one address (`at`), or a `table` of
`entries`, each `entry_size` bytes with the list's address at byte `pointer`
of it, low byte first (and the list's bank, if not the table's, as
`stream_bank`: a number, or `{"byte": N}` for a byte of the entry). A
start's grammar is a name, or told by a byte of the entry (`byte`, `mask`
and `is`), as where an entry says which channel its list is for.

**Following.** `streams` reads every start through its commands and through
what they call, jump and loop to, in every bank. A byte no command of the
grammar starts with stops that list there, and so does a command that would
leave its bank, a target that is not in the ROM, and a list that leads into
the middle of a command read from elsewhere. Each is said with its address,
nothing is guessed, and what was read before it stays. The command says how
many starts were read to where their commands end them, and names an entry
of a table that leads nowhere. A place two grammars both read is written in
the first and counted.

**The files.** A *passage* is a run of whole commands in one data file, from
a place something points at (a start, or a target) or where a list comes
into the file, to the next such place. `streams` writes each region's
passages to `<file>.stream.txt` beside it, and records it under `views`
(`kind` `stream`, with its `passages`: where each starts in the region's
file, its size, its grammar):

```
# 02:4B2F Music_Town_Ch1
02:4B2F music 9
  tempo 144
  octave 4
  note C_ 4
  call 02:4C10
  loop 0 02:4B2F
```

The line a passage starts with is its label: its address, its grammar and
how many bytes it has. A target is written as the address it leads to, so a
line that says `call 02:4C10` goes to the passage that starts `02:4C10`,
whichever file that is in. Where the tree has names (*Names for the files*)
the names of a passage's address are in a comment over it. Edit the
commands and leave the passage lines. `build` and `status` read the file as
they read a file of text or of tile numbers:

- a file as `streams` wrote it changes nothing, and neither does one that
  still says the bytes it was written from, however it was saved again;
- commands that make as many bytes as the passage has are written over it,
  and the region's file is not written to;
- **a passage cannot grow where it lies.** More bytes are refused, with how
  many too many. Moving a passage needs every pointer to it turned: the
  entry of the table, and each command that calls or loops to it;
- fewer bytes are written where the passage's last command ends the list
  (an end, a jump, a loop that never stops), and what was after them stays
  as it was, since nothing reaches it. Where the last command goes on to
  what follows, a shorter passage would have the engine read old bytes as
  commands, and is refused;
- a line that says what the grammar cannot is refused with its file and
  line: a command or a name it does not have, a value that does not fit, an
  address the list cannot reach, a command whose first byte would be
  another command's;
- if the region's file was edited in those bytes as well and says something
  else, `build` stops and names both. `streams` does not write over a file
  somebody edited, and says so; `--force` writes it again from the region's
  file.

A target can be changed to any address in reach, of a passage or not.
`tiles` does not draw over commands, and `streams` leaves out a passage
whose bytes a picture or a file of text has.

**Commands in files of code.** The analysis decodes some music as
instructions, as it does other data (*Telling the analysis what is data*),
and such bytes are in files of code, which no image can change. `streams`
reads through them all the same, counts them, and writes no text of them;
a command that lies half in one file and half in the next is in neither's
text. `streams --annotations FILE` writes every run of commands as `data`
lines for `gbrecomp --annotations`. With the game and the export made again
with them, and the tree split again, those bytes are data.

**The check against the game played.** A declaration is a claim, and a
record says whether it holds without a disassembly to compare with. An
engine reads its commands through one or two instructions, and `uses` has
written which instruction read each byte first. So where the tree has
records, `streams` reports, for the instructions that read commands: how
many of the bytes they read lie in decoded commands and how many do not,
with where; and of the decoded bytes, how many they read, how many another
instruction read first, and how many no record saw read. The instructions
are the `readers` of the declaration, or `--reader BANK:ADDRESS`, or found:
those that read at least 16 bytes, more than half of them decoded. A byte
those instructions read that is not decoded is a start nobody declared, or
a grammar that stops short; a decoded byte another instruction read is a
start that is not a list of commands.

**Names, and a tree split again.** The file is renamed with its region
(`names`). `split --keep` carries it where its region is the same bytes in
the new tree, with `streams.json`. Where the region is cut another way it
is left behind and named, the edit made through it is in the bytes of the
new files, and `streams` on the new tree writes it out again from them.

What this does not do:

- **Find the engine.** Somebody writes `streams.json`: from a disassembly,
  or by reading the engine. `uses` shows which instructions read bytes that
  reach the sound registers, which is where to start reading.
- **Move a passage.** A tune can be rewritten in the room it has. It cannot
  get longer, and a new tune cannot be added.
- **Lists nothing leads to.** A list that no table and no command points at
  (one the game's code points a channel at by itself, or one that is never
  played) is not found until it is declared with `at`. The check above
  shows the ones a record saw played.
- **Anything but commands.** The table of starts itself, wave samples and
  the pitches of the notes are not views. What a number means (which
  instrument, which sound) is whatever the declaration names.
- **The one command.** `gb_workspace.py` does not run this, and takes no
  `streams.json`.

## On Pokémon Red

With an export made with its far calls, 49 declared tables and played
evidence (`LIFTING.md`): 5,377 files, of which 2,709 are data (829,277
bytes), 2,667 code (219,223 bytes) and one the header. `build` makes the
original ROM from them (SHA-1 `ea9bcae6…`) in half a second. A game given
that image ends 1,500 scripted frames with every register, counter and byte
of RAM as it has them without. With three bytes of `bank_01/5D7E_unknown.bin`
edited, the main menu's first line reads MOD GAME.

A record of its first 1,500 frames (the intro, the title screen and the
start of a new game) follows 9,887 bytes. Of the data, 4,964 bytes go to the
tile area as they are, from 49 regions and nearly all through one copying
routine; 131 reach a tile map, most of them worked out; 179 go to sound
registers and 43 to the sprite table; 18 are halves of addresses the game
jumped to and 571 of addresses it read through. And 2,844 of the bytes it
read as data lie in what the analysis had decoded as code. The recorded run
ends in the same state as a plain one.

Fifteen seconds recorded from each of 204 save states, the ones the game
made at every checkpoint while exploring itself as far as its fourth badge
(`LIFTING.md`), take ten seconds in all and add up to 73,827 bytes of data
read, 9% of it: 32,094 go to the tile area as they are, from 289 regions,
7,148 to a tile map, 1,025 to sound registers, and 213 are halves of
addresses jumped to. By the instruction that read them, with the
disassembly's name for the routine each is in:

| Read at | Bytes | What became of them | The disassembly's name |
|---|---|---|---|
| 00:00B5 | 33,339 | 31,335 to the tile area as they are | `CopyData` |
| 00:2693 | 8,982 | none arrives anywhere as itself | `ReadNextInputByte`, of the routine that unpacks pictures |
| 00:0A32 | 5,288 | none; a fifth used to look something up | `LoadTileBlockMap` |
| 08:5FF1, 1F:58A6, 02:5832 | 8,662 | a ninth to sound registers as they are, half used to look something up | `Audio1`, `2` and `3_GetNextMusicByte` |
| 00:1956 | 3,660 | 3,322 to a tile map as they are | `PlaceNextChar`, which prints text |
| 00:37B8 | 2,120 | 163 to a tile map | `GetName` |

So the reader says what the bytes are where their fate does not: packed
pictures and music are told by who reads them. 23,843 of the bytes read as
data lie in what the analysis had decoded as code.

For pictures, its tiles were found with the disassembly's symbol file, not
with a record. `RedSprite` (05:4180) lies whole in
`bank_05/4146_unknown.bin`, and its 24 tiles drawn two to a row are the six
frames of the sprite. With seven pictures drawn, of that sprite, two whole
tilesets, and parts of a third and of the font, `build` still makes the
original ROM. One pixel of the sprite painted black with Pillow, and the
picture saved as RGB, changes two bytes of the image, at offsets 0x14180 and
0x14181.

And the game shows an edited picture. With the two middle greys of that
sprite's picture changed about, the image differs from the ROM in 294 bytes,
and a new game played from switching on to the first room, by the same
buttons with and without `--rom-image`, shows the same frame but for the
player, whose cap is light where it was dark.

For colours, the 204 save states were recorded again by a game that looks at
its screen (14.0 and 15.1 seconds, against 12.7 and 14.3 for the game before
on a busy machine; every record is the earlier one with `shown` added, and
every run ends as the same buttons played plain do),
and with them a new game's first 4,200 frames. Of the 42,448 bytes that went
to the tile area as they are, 21,464 were on the screen, in 795 runs, 148 of
them (5,503 bytes) drawn by sprites. Seen longest was E4 for the background
(10,616 bytes) and D0 for sprites (4,064), which draws colours 0 and 1 both
white. For 5,120 bytes of background it was 00 or FF, all white or all
black, which is the screen while the game loads, and for 496 bytes FE, the
dark of a cave walked through without a light: those are the palettes
`tiles` passes over where it has another. Sixteen overworld sprites drawn as
one picture (05:4180, 384 tiles, two to a row) come out through D0 with
colour 0 clear. Saved again as RGBA, the tree is still the original ROM. With
the player's two darker shades changed about and a solid white square painted
into a clear corner of each of its six frames, saved as RGBA, 187 bytes of
the image differ and a new game shows the player so. The same picture
flattened to RGB makes 184 bytes: there the squares cannot be told from what
is clear, and stay clear. The player's own tiles are in no record, since the
game copies them with the stack pointer in the ROM, which the recorder does
not follow; they got their palette from the sprites beside them in the
picture.

For tile maps, the same 205 records have 11,262 bytes in a cell of a tile
map on the screen as the ROM has them, in 2,234 rows. Without what the three
instructions that read text read (00:1956, and 01:4377 and 01:6785 of the
naming screen), `maps` writes 674 grids, 6,937 bytes, into 58 files, and the
tree is still the original ROM. 323 of the grids are four tiles by four,
which is a block of this game's maps; 75 are four by two and 179 two by
one, pieces of blocks that were only ever at the edge of the screen, where
the game shows half a block. The block set its towns are built from
(`Overworld_Block` in the disassembly, 128 blocks at 19:45E0) lies in one
file with its tile set and four more sets: 1,724 of its 2,048 bytes are in
grids, 96 of them whole blocks, and the picture draws them with the town's
tiles. 1,254 of those bytes were seen to draw a ROM tile; the other 470 were
on the screen only with tiles that were in video memory before a save state
was made, and are drawn by the nearest byte that was seen to draw the same
number. With one block of the first room given another's sixteen
numbers in the file of tile numbers, `build` changes those 16 bytes, and a
new game shows the room standing in a field of beds where its dark surround
was.

Its tiles are found without the symbol file as well (*Tiles found from a
played game*). The 204 save states and a new game's opening were recorded
again with `--data-use-tiles`: every one of the 205 runs ends in the same
state with the recorder off, with it on, and with the option, and each
record is the one made without the option with `tile_runs` added. In all
45,608 bytes land in the tile area as they are, in 196 runs; 12,704 of them
the game reads with `POP`, in the two copies it makes during vertical
blank. On a tree nobody has told of any tiles, `tiles` with no
file named makes 120 pictures of them: 2,807 tiles, 43,720 bytes, 114
pictures at 2 bits a pixel and 6 at 1. Two are the two that had been told
by hand from the symbol file, found the same: the font, 128 tiles at 1 bit
a pixel from 1,856 bytes into its file, and the overworld tileset, 94
tiles, though the game copies 96 (the last two are the start of its blocks,
which the game also draws maps from). With those two in the tree it writes
the other 118 (2,585 tiles, 41,192 bytes), and says the two are there. Run
again, it writes nothing, and `build` makes the original ROM.

By the disassembly's labels, with those 205 records:

| | One picture | Whole in a bigger picture | In pieces | No picture |
|---|---|---|---|---|
| Overworld sprites (67 sheets, banks 4 and 5) | 31 | 12 | 8 | 16 |
| Tilesets (25 `_GFX` labels, 20 different sheets) | 9 | 8 | 0 | 8 |
| The font | 1 | | | |
| The HP bar, the text box and the logo | 2 | | 1 | |

The sprites in a bigger picture are sheets of twelve tiles or fewer and the
sheets after them: the game loads twenty-four tiles for every sprite that
walks, so what lies after a short sheet comes along. Those in pieces: the player's own, whose two halves
the game reads at addresses its code spells out; three whose second half a
table of icons has the address of; three with bytes still in files of code;
and one whose first half no record shows. The tilesets in a bigger picture
have a tile or more of their blocks after them, where no record shows those
blocks drawn. The logo is two pictures, read by two copies at two addresses
of the code. What has no picture was not loaded in any record.

Of the bytes that landed, 1,018 are in no picture: 712 are what the game
copies after a short tileset, from the first byte it also draws a map
from; 288 are ends of runs that are not a whole tile, 224 of them in the
bank of the battle animations' tiles, which files of code still cut up; 16
are a table the game looks pixels up in, which land all over a tile; 2
landed alone. 868 bytes of tiles are in 27 files of code. And four
stretches between two runs of tiles in one file are in no record in any
use, 800 bytes: two whole sprite sheets nobody met, the first half of a
third, and two tiles after the font. None is the unseen middle of a sheet,
which is why such stretches are left out.

With the two middle greys of the picture found at 19:4DE0 changed about,
the tileset of the first room, the image differs from the ROM in 1,032
bytes, all of them that picture's, and a new game played from switching on
by the same buttons shows the room's floor and furniture in the other
shades, with the registers and the cycle count as they are without.

For text, the rule finds one instruction in the 204 records, 00:1956
(`PlaceNextChar` in the disassembly), and leaves out the four that draw the
map from blocks (00:0F38 to 00:0F41, 926 bytes each, every one to the tile
map). The letters were read off `glyphs.png` drawn from a save state in the
overworld: 97 bytes, capitals from 80, small letters from A0, digits from
F6, and names were given to 13 bytes that draw nothing. With a record of a
new game's opening added, `text` writes 135 strings, 4,288 bytes, into 24
files (a second instruction, 01:6785, reads the letters of the naming
screen), and `build` still makes the original ROM. With two strings edited
in `bank_22/4000_unknown.text.txt`, one to as many bytes and one to four
fewer, a new game played by the same buttons shows the professor welcoming
the player to "the world of MODS!" and, at the console in the first room,
"...Okay! Go on, edit!". That is a small part of the game's text: the
records are of buttons pressed at random, which seldom talk to anybody, and
its names of Pokémon, moves and items are copied before they are printed.

By itself the analysis leaves most of its sprites and tilesets in pieces:
it decoded stretches of them as instructions, and those stretches are `code`
files. Told what the 206 records show (`annotations`), it leaves them alone:

| | By itself | Told |
|---|---|---|
| Overworld sprites lying whole in one data file (67, banks 4 and 5) | 20 | 52 |
| Their bytes in files of code (of 19,392) | 3,094 | 678 |
| Tilesets lying whole in one data file (25 `_GFX` labels, banks 19 to 1B) | 9 | 18 |
| Their bytes in files of code (of 24,576) | 3,267 | 723 |
| The font, the HP bar, the text box and the logo, whole (4) | 0 | 4 |
| Data in the tree, bytes | 829,277 | 851,998 |
| Bytes the game read as data that are in files of code | 24,443 | 8,971 |
| Functions in the export | 6,523 | 6,247 |
| The disassembly's 2,318 functions that are functions in the export | 2,159 | 2,159 |
| The disassembly's data that the export decoded as code, bytes | 80,673 | 60,666 |
| The game's executable, bytes | 8,995,328 | 8,143,360 |

The overworld tileset (`Overworld_GFX`, 94 tiles) was in 54 files, 27 of
them code, and the font (`FontGraphics`, 128 tiles at 1 bit a pixel) in 20;
each is in one data file now, and `tiles` draws it whole.

The game made with the annotations holds an image to 196,578 bytes in 2,669
ranges: the tree's 196,502 bytes of code and the 76 of the header. It held
217,558 while an address with nothing decoded at it was still made a block.
Such a block has no end, and one of them, at 01:409C in a table of sprite
positions, was given the end of the block before it, the routine in high
RAM: everything from there to offset 0xFF8A counted as code, 20,980 bytes of
194 data files. `build` took an edit to any of them and the game refused the
image, the main menu's NEW GAME among them, and 196 bytes of 11 of the 135
strings. There were 137 such blocks, each an empty label in the generated C:
84 jumped to themselves and 5 went on into other code. Without them the C
has the same 119,938 instructions, and the game ends 4,300 frames from
switching on, and 900 from each of six of the save states, as it did, with
the interpreter run at the same places. The game made without annotations
had 144 such blocks, none of which happened to reach into data.

The game used 58,239 bytes as they are, in 2,736 runs, and 14,008 of those
bytes had been decoded. None is left in a file of code, and the annotations
made again from the new tree are the same file. No function the disassembly
has is lost; the 900 bytes fewer that the export decodes of what the
disassembly calls code are the empty end of bank 4. The game made with the
annotations ends as the game made without them does, to every register,
counter and byte of RAM, and falls back to the interpreter exactly as often:
over a new game's first 4,300 frames, over 900 frames from each of the 204
save states by the buttons the records were made with, and over 1,800 from
each by others. Lifted from the new export (`LIFTING.md`), 4,445 functions
check as matching and none as a mismatch, where 4,459 did: the 14 fewer were
decoded out of data. The lifted game spends the same 140,990,536 cycles of
3,600 frames in lifted code, 97.9%, and ends as the original does. The
sprites and tilesets still in pieces are ones no record shows on the screen.

They need not be played to. Started from one save state at the routine that
enters a map, once for each of the game's 222 maps (247 runs of 240 frames,
5 seconds), the game loads every tile set and every person's sprites, and
the records of those runs with the 206 above make annotations that leave 66
of the 67 sprite sheets whole and every tile set (23 of the 25 labels; the
other two are second names for one tile set's bytes). The last sheet, the
player on the bicycle, the game copies in the vertical blank with the stack
pointer in the ROM and `POP`; with such reads followed it is whole too, and
133 of the disassembly's 149 uncompressed graphics are, where 108 were. The
game made with those annotations ends as the one above does from a fresh
boot, from the 204 states and from the 247 map loads, and the lift check
has no mismatch. The 16 left are drawn by nothing in those runs: the slot
machine, the trade, fishing, and the like.

Its Pokémon and trainers are not helped by this. They are packed, so no
byte of them is used as it is: of the 152 front pictures' 56,310 bytes,
12,163 are in files of code either way. The records show the game unpacking
28 of them, 2 of the 153 back pictures and 10 of the 46 trainers.

They are edited as what the game unpacks. Following 00:2693 over the 204
save states, `unpackers` proposes the routine at 00:251A: all 82 of its
calls that the records settle read one stream whole, the word at D0AB held
the stream's address and register B its bank in every one of 80 samples, of
30 streams in 7 banks, and all 80 left 784 bytes at A188 in cartridge RAM.
That routine is called from one place, by a routine at 00:24FD that takes
the bank in A and that nothing calls, only jumps to, which is why the
recorder names the inner one. Either declaration harvests the same bytes.
The 784 bytes are the two bit planes of a picture, 392 bytes each, a column
at a time, and the stream's first byte is the picture's size in tiles:
that much is written into the declaration by hand, to have pictures. With
that declared, a new game's opening and four rounds of buttons at random
from each of the 204 save states (820 harvests, a minute and a half of
running) give 93 different things made of 74 streams: 53 of the 152
Pokémon from the front, two from the back, 17 trainers and people, and the
player, each a PNG of its own size. Nineteen of the streams were unpacked
two ways, and in each the second is the first with the bits of both halves
of every byte turned about, which is how this game makes a picture to show
mirrored: `unpacked` finds that from the bytes, keeps the pairs in step, and
works out the other way for the 55 streams harvested one way only. Of those
that had only been seen mirrored, the form worked out is the one that looks
like the picture. What is not there yet is what nobody has been seen to
meet: a played run covers more than buttons at random do.

With dark glasses drawn on the professor and the player's picture turned to
its opposite shades, `build` makes the original ROM and a pack with two
replacements, and a new game played by the same buttons shows both, in the
recompiled game and in the one with 4,445 functions lifted. The game given
the pack that only watches ends 4,300 frames in the same state as the game
given none, and so does the game given the edited pictures, in every
register, counter and byte that is dumped.

From the hub (`hub/README.md`, *Edited data*): the hub imports and builds
the game in 36 seconds, and its *Make data tree* job takes 8 seconds to
write a tree made with the options it builds with (`--pointer-scan
strict`): 4,975 files, 2,508 of them data. With that tree, and the
professor's
"world of POKéMON!" changed to "world of MODS!" in
`bank_22/4000_unknown.bin` (four bytes differ), the hub builds the tree and
starts the game with the image, and a new game played by a script shows the
new line; with nothing chosen the same start shows the old one. With a file
of code edited the game is not started and the hub shows the tool's message;
with an image changed at offset 0x150 it shows the game's.

For data that grows, one run was recorded with the pointers: a new game
from switching on, through the professor's speech and the naming screens
into the first room, 4,400 frames. The record without its `pointers`
section is, byte for byte, the record the game made before the recorder
knew of pointers, and the recorded game ends as the plain one does. In it
169 pairs of bytes made addresses the game read its ROM through: 61 read as
data, and 108 the operand of an instruction. They lead to 136 things in
data files, 14,075 bytes of them read:

| | Things |
|---|---|
| Can move: every read came through pointers that are data (57 of them) | 52 |
| An instruction's operand leads to it | 80 |
| Read through an address no two bytes of the ROM made | 2 |
| Its pointer lies in a file of code | 1 |
| Its pointer is a number to the game as well | 1 |

`free` proposes the ends of 51 of the 64 banks, 455,381 bytes: 19 banks
that are empty altogether, about 5,000 bytes in each bank of text, and
none in the two banks its music is read from in this run.

`build --move-all` moves 16 of the 52 into that room, 2,783 bytes, turns
17 pointers, and spoils the 606 bytes the game read at the old places. The
other 36 had no room in their own bank: 31 are sound data in two banks
with nothing free, and five are larger than what is free in theirs. The game
played from that image by the same buttons shows the same picture in each
of 99 frames spread over the run, plays the same 74 seconds of sound,
sample for sample, and ends with every register, counter and byte of RAM
as the original has them but two: the address of the first room's map,
which moved.

The first time, it did not. Seventeen things moved, the frames were the
same, and the game ended 9,648 cycles apart from the original with other
numbers in its random bytes. Moving one thing at a time found it: three
bytes that say a map has no objects to show and hide. The game takes the
start of a table from the pointer to them and divides by three, to know
which object the map's first is. That pointer is a number to the game as
well as an address, and nothing a pointer's bytes were *used for* said so.
The recorder marks such bytes now (`numbers`), and that thing stays.

For a string that grows, the professor's first speech was made 31 bytes
longer: after "world of POKéMON" it goes on for two more lines. `build`
moves the 90 bytes it is part of into the free space of bank 22 as 121,
and turns one pointer, at 01:6254. The game shows the new lines, and after them "My
name is OAK!", the rest of that speech and the speeches after it as they
were.

The same work found that a game generated with annotations refused any
change in a stretch of 48,878 bytes of the ROM, the data in it included.
An entry point
that lay inside declared data made a block with no instructions in it,
whose end address was never set, and the generator wrote what that end
address happened to say into the ranges an image has to match. It writes
nothing for such a block now, and the game's ranges are the tree's files
of code exactly: 2,669 of each.

The game was then had to unpack the rest by itself (`unpack`). Its
declaration says to enter 00:36EB, which takes the stream's address in DE
and its bank in A, with the byte at D0AA nought, and once more with it 1
for the mirrored form. Given the 355 addresses of its packed pictures as a
list, in a save state from its first badge, it writes 355 harvests in about
three seconds, eight copies of the game at a time. Each is the same file,
byte for byte, as the harvest a script written for this game alone had made
by patching a save state, which took eleven seconds. The game as it is
switched on gives the same files, and so does the declaration with no
`call` in it, which enters the routine at 00:24FD itself. With the variant
there are 710 harvests in five seconds, and their 710 things are the ones
the tree had: the 321 mirrored forms that had only been worked out are what
the game makes. Its table of trainers, declared as 47 entries of five bytes
whose streams are in bank 13, names 45 streams, all of them among the 355.

Walking on from the 74 streams play had shown, with no list, finds 333 of
the 355: the 74, 237 in chains that arrive at another known stream, and 22
that end in the open, in `guessed`, which are the last pictures of six
banks. All 333 are, byte for byte, what the list gave. The 22 it does not
find lie before the first stream play showed in their bank (twenty, in
banks 09, 0B and 0C) or in a bank where play showed none (the two of bank
01). It also makes seven things that are no pictures, all of them in
`guessed`. Where the pictures of banks 04, 0A, 0C and 13 end, code follows,
and the routine made something of one to three stretches of it before a
size no picture has, or bytes play had read as something else, ended each
chain. All seven lie wholly in files of code, as the summary says; of the
259 real streams it found none does, though one has 145 of its 146 bytes
there. The walk takes seven seconds, and more on a busy machine.

A picture found only that way is in the game. The title screen, left alone
for 6,000 frames, unpacks eleven pictures, four of them not among the 74.
One of the four, at 0C:5D31, was found by walking on and borne out. With
its picture turned to its negative, `build` makes the original ROM and a
pack with two replacements, the title screen shows the negative from frame
3,180 on, and the game ends the 6,000 frames in the same state with the
pack as without.

Named from the disassembly's symbol file (`names --sym`; the file stays
outside the repository), that tree has a line in `names.sym` for 8,251 of
the 17,099 addresses the symbol file has in the ROM. The rest are inside
code. All 710 unpacked things are called by a name, two to each of the 355
streams: `PikachuPicFront.png`, and `PikachuPicFront_2.png` for the form the
game mirrors, 321 of which were worked out and never harvested. So are both
pictures of tiles, and 868 of the 2,717 data files, which hold 327,462 of
the 851,998 bytes of data: 442 by the one name they have in them
(`Label.bin`) and 426 by the first of several (`Label_and_N_more.bin`).
That is 2,290 files renamed, in under two seconds. Of
the data files that keep their address, 489 have names further in and none
on their first byte, since a region starts where the analysis stopped
decoding and not where the disassembly put a label: the overworld sprites
start at the 59th byte of `bank_05/4146_unknown.bin`, which lists them,
`RedSprite` first. The other 1,360 have no name in them at all; 697 of
those are of eight bytes or fewer, gaps in what the disassembly has as
code, and 290 more are padding. Of the 426 files with several names the
largest is bank 22, one file of text with 315 names in it:
`_RockTunnelB1FHiker3AfterBattleText_and_314_more.bin`. A copy of the tree
that had been named when such a file went by its first name alone has
those 426 renamed by the next `names`, and is then the same tree, file
for file. The tree builds to the original ROM named as
unnamed, and to the same pack. With Pikachu's picture and the font turned
to their opposite shades, the image and the pack are the same three ways:
in a tree nobody named, edited and then named, and named and then edited.

The older tree, the one with the text, has 817 of its 2,709 data files
(407 by one name, 410 by the first of several) and
23 of its 25 pictures and files of text under a name, and 134 of its 135
strings: 22 start at a label and 112 after one, 81 of them one byte after,
because the disassembly's label is on the byte in front that says text
follows. Without the symbol file, `--seen` calls 614 of the data files by
what the game used them as: 316 `pointer`, 105 `tile_map`, 79 `jump`, 51
`sound`, 31 `tiles` (all 42,448 bytes it drew as they are), 17 `oam`, 12
`bank` and 3 `palette`.

A save state made with the ROM loads in the game given an edited image.
The 204 save states of the play-through were made with no image, by a
runtime from before states went with any image. The game given the image
`build` makes of an unedited tree ends 900 frames of buttons at random from
each of them as it does given none, in everything that is dumped, and says
nothing. Given an image with a string and the player's sprite edited, and
the pack with one picture, it loads all 204 and says of each that it was
made with another image, where the runtime before refused them. From the
state in Saffron City the player still has his dark cap as the state loads,
since his tiles are in video memory; asked whether to save, the game prints
"SAVE the MODS?", reading the string again, and after that screen it has
loaded the sprite again and his cap is light.

Split again, a tree keeps its work. A tree of the export made by itself,
with that string changed, the sprite's picture repainted (294 bytes) and one
of its 355 unpacked pictures drawn in its opposite shades, taken into a tree
of the export told what the records show (`split --keep`): the picture, the
24 files of text with their 135 strings, the 355 things and the charmap are
all there, two of the 25 views under another name or at another offset, and
`build` makes the same image and the same pack from both, byte for byte.
What `uses` wrote is not carried, since 22,742 bytes are data that were
code, and is written again from the records. Taken back into a tree of the
first export, every picture, text and unpacked file is the one it started
as. Going that way, the tree split with the annotations loses both of its
pictures of tiles, the font and the overworld tileset, 659 and 476 of whose
bytes are code again, and the command names them; and with three bytes
edited in that tileset where the export made by itself has code, it stops
and writes nothing until it is told to go on without them.

All of it in one command (*The way in: one command*): from the ROM, the 204
save states, the charmap, the declaration of the unpacker with its table of
trainers, the disassembly's symbol file, and the options the game above was
generated with, its annotations given as lines of one's own.
`gb_workspace.py` takes 3 minutes and 12 seconds, six copies of the game at
a time: 17 seconds to generate the game and 22 to build it, twice each, 16
for each export, 15 to split the tree again, and the rest for 206 records
(two from switching on and 900 frames from each state, which were made
without sound and are played so), as many harvests, and the views. The
analysis is told 61,532 bytes in 2,832
runs, where the lines given had 58,239 in 2,736. The tree has 2,700 data
files (852,933 bytes) and 2,654 of code, and builds to the original ROM.
In it:

| | From one command | By the steps before |
|---|---|---|
| Pictures of tiles | 114, 2,796 tiles | 120, 2,807 tiles: 118 found and the two told by hand |
| Tile maps | 669 grids, 6,857 bytes, in 58 files | 674 grids, 6,937 bytes, in 58 files |
| Text | 156 strings, 4,122 bytes, in 52 files | 135 strings, 4,288 bytes, in 24 files |
| Packed streams play showed | 41 | 74 |
| Streams of the table of trainers | 45, 36 of them not in play | 45 |
| Lines in `names.sym` | 8,341 | 8,251 |
| Data files called by a name | 867, and 405 more by what the game used them as | 868 |

The differences are in the play. The steps before had a new game's opening
recorded as well, by buttons chosen for this game, which no tool for any
game can press: it has two runs from switching on in its place, one with
nothing pressed and one with buttons at random. The 38 streams its one round
from the states shows are the 38 the first round showed before, to the
stream; the 74 took four rounds and the opening, and the three later rounds
each showed more than the first did (55 to 57 streams), so they were other
play than a round is here. The text is in more files because the tree it was counted in before was split
without annotations, and has other strings in it because the opening is
other play.

The game the command built ends as the game above does, in everything that
is dumped: 4,300 frames from switching on by the same buttons, and 900
frames from each of five of the save states. Run again, the command runs
nothing, in three seconds. With the font's picture
(`bank_04/FontGraphics.tiles.png`, named by the symbol file) turned to its
opposite shades, the build command of the README makes an image that differs
in 1,024 bytes, and the game started by the README's other command shows the
professor's speech in letters of the opposite shade. Run once more on that
folder with `--rounds 4`, it records the 612 runs that are new, tells the
analysis what they show, makes the game again and splits the tree again, in
3 minutes and 21 seconds: the edited tree builds to the same image, to the
byte, the game shows the same frame, and the tree has 118 pictures of 2,942
tiles, 222 strings and 52 streams from play.

**Music and sound** (*Music and sound as commands*), on a copy of that
folder's tree, with 338 records in its manifest. The declaration has four
grammars: the music's three melodic channels (17 commands), its drums, the
sound effects, and the effects' noise channel, which change 8 commands
between them. Its starts are the three tables of 254 entries that the three
copies of the engine have, in banks 02, 08 and 1F.

`streams` follows the 762 entries, reads every one to where its commands end
it, and decodes 25,004 commands in 36,186 bytes, in under two seconds: 46
pieces of music on 151 channels and 316 sound effects on 611. 33,698 of the
bytes are in 1,078 passages of 137 data files, 26,370 lines of text. 2,052
are in files of code and 436 in commands that lie in two files; of the 762
channels, 694 start in a passage that can be edited. The tree builds to the
original ROM as before.

The check found the instructions that read commands by itself (02:5832,
08:5FF1, 1F:58A6), and found what the tables do not lead to. With the tables
alone, 22,492 of the 22,582 bytes those three read were in decoded commands
and 90 were not, in eight runs. Each run was a start to declare with `at`:
six places in bank 02 that the game's code points a channel at, for a tune
played from another beginning or at another tempo, and one `sound_ret` among
each engine's own code. One of the six lay inside another's run, and showed
when that one was declared. With the nine declared, all 22,582 bytes are in
decoded commands. Of the 33,953 decoded bytes in data files, no record saw
11,362 read, which is the music and the effects nobody played, and another
instruction read 9 first.

Held against the disassembly, whose macros the grammars were written from:
of its 779 labels for a channel of a tune or an effect, 762 lie in decoded
commands. The 17 others are lists no table leads to and no record saw
played: an unused song, an unused fanfare, the three unused cries each bank
has, and the flute's, which the game's code points channels at. They are
there to declare. The bytes of the audio banks that are not decoded
otherwise are the engines, their tables, the wave samples, and ten single
bytes in bank 02 that nothing reaches: a command that ends a list, nine
times after a loop that never stops and once after another such end.

One edit, to hear it. In `bank_1F/77FF_sound.stream.txt` the passage at
1F:7844 is the first channel of the tune the game opens with. `tempo 98`
made `tempo 196`, and `octave 2` before its first two notes made `octave
3`, is two bytes of the image. The game given that image and the game given
the ROM, each played 2,400 frames from switching on with the sound
hardware's writes traced (`--debug-audio-trace`): the wave channel is given
the same twelve pitches in both, with 1.90 to 2.10 times as long from one
note to the next in the image, 2.00 on average, and the first channel's
first note of the tune is 441 Hz where it was 220.

## Not done

- **Growing what an instruction leads to, or into another bank.** A thing
  moves only if every pointer to it is data (*Data that grows, by moving*).
  What an instruction's operand leads to, which is most of a game's data,
  stays where it is until the instruction is lifted C that names the data
  and not an address. A thing stays in its bank: the byte that says the bank
  is not yet followed to the pointer it goes with. Only a string grows: a
  picture or a table has no way yet to say that it is longer. And a file of
  the tree still keeps its size and its place: what moved is a copy in free
  space, made again by every build.
- **Knowing what a region is.** A region is a run of bytes between code, and
  its name a guess from the bytes. `uses` records what the game did with
  them, `text` finds strings from that, and `tiles` with no file named
  finds pictures of uncompressed tiles (*Tiles found from a played game*).
  How a picture's tiles are put together on the screen is not found. Tables
  as records are a separate step, and are not built. `names` calls a file
  by the label a symbol file has for it, or by what the game was seen to
  use it as; where there is neither, nothing says what a file is, and
  names for functions, RAM and constants are not in the names file yet.
- **Data the analysis takes for code, where no record says otherwise.**
  `annotations` tells the analysis what a played game used as it is. Data no
  record shows in use stays as the analysis has it, and so does data that is
  never used as it is: packed pictures, and anything else the game only
  works something out from. Saying those are data takes knowing more than
  that they were read, since code is read too.
- **A tree split again, in full.** `split --keep` takes a tree's work into
  the tree of a new export (*A tree split again*). A picture or a string
  that the new export cuts in two is left behind with its edit in the bytes,
  and what `uses` wrote is carried only where no code has become data: after
  the analysis is told more, `uses` is run again.
- **Other graphics.** Sprites are not put together: a sheet shows tiles in
  the order they lie in the ROM, and which of them make a sprite is in the
  object table, which a game works out and does not copy. Tile maps are
  found as far as their bytes were seen on the screen (*Tile maps as tile
  numbers*), and a picture has one palette, the one its tiles were seen in
  (*Tiles in the game's colours*). A Game Boy Color's attribute maps, which
  give each cell its palette, are not views.
- **Finding every packed stream.** `unpack` has the game unpack the streams
  of a list and of a declared table without playing, and walks on from the
  streams that are known. A stream that lies before the first known one of
  its run, or behind a table whose bank cannot be declared, still takes a
  list written by hand: nothing walks backwards, and nothing finds a table.
- **Launchers.** A project generated from a directory of ROMs takes
  `--rom-image` for the game it starts; it has not been tried.
- **The one command, all the way.** `gb_workspace.py` runs every step that
  nobody has to read anything for (*The way in: one command*). Left to do by
  hand after it: the records and the two commands for data that grows
  (`--data-use-pointers`, `pointers`, `free`), finding the routine that
  unpacks, walking on from the streams that are known, and new strings for a
  file of text that was edited. Where no play is given it presses buttons at
  random, which shows little of a game. It has not been run on a Game Boy
  Color game, and the hub's *Make data tree* job does not call it yet.
- **Music that grows.** A passage of commands is rewritten in the room it
  has (*Music and sound as commands*). One that got longer has to move, and
  every pointer to it with it: the entry of the table it starts from and
  each command that calls or loops to it, all of which `streams` knows, and
  any place the game's code points a channel at, which it does not. That is
  the work of *Data that grows, by moving* with other pointers, and is not
  built; nor is a tune of one's own added to a table. The declaration is
  written by hand, and the one command does not run `streams`.
