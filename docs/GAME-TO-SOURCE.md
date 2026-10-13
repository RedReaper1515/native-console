# From a ROM to the game written in C

This is the plan for taking one Game Boy game from its ROM to source code a
person would call the game: C functions with arguments, the game's variables
by name, its pictures, text, maps and music as files. It is written to be
followed again for the next game. Each stage says what is done, with which
tool, what proves it, and what in it is the game's own work and what is the
same for every game.

The tools are in `LIFTING.md`, `DATA_FILES.md` and `RUN_RECORDS.md`. This
document is the order to use them in, the gates between the stages, and the
parts that are not built yet.

## Whether it can be done

Yes, a function at a time, and not in one go.

A game is not rewritten and then debugged. It is recompiled first, so that
it runs exactly; lifted, so that every function is C that can be compared
with the ROM; and then each function is written again as real C in the place
of its lifted one, while the game stays playable and every function is held
to the original by the checker. A wrong function is found the day it is
written, by name, with the state it went wrong in.

What makes that true was measured on Pokémon Red
(`LIFTING.md`, *Functions written by hand*): nine functions written as
ordinary C, each matching the ROM in every state tried and taking the same
time on every path, and the game with them ending 108 runs of 3,000 frames
with every byte of its RAM as the original has it.

What it costs is the writing. Pokémon Red is about 76,000 lines of assembly
that are code (48,000 of engine and home bank, 23,000 of map scripts, 5,000
of sound engine) in some 2,300 functions, beside 65,000 lines that are data.
Nine functions are done. The rest is a pass a model makes with the checker
in the loop, and its size is the open number: the nine were written while
the method was being found, so they say nothing about the rate. The first
hundred written under the rules below will.

## What done means

Three different things are called "the game in C". They are stages 5, 7 and
8 here, and each is a place to stop.

| | What the source is | What it still leans on |
|---|---|---|
| **Stage 5** | Every function is C with arguments and named variables. | The Game Boy's memory map, its video and sound chips as a library, and the original's timing. Byte for byte the original's behaviour. |
| **Stage 7** | The same, built from files: pictures as PNG, text as text, maps, music. No ROM image, no banks. | The Game Boy's video and sound as a library. Still the original's behaviour. |
| **Stage 8** | A program of its own: structs, a renderer, frames. | Nothing of the console. No longer comparable with the original byte for byte. |

## The stages

Each has a gate. A stage is not finished by being worked on: it is finished
when its gate has been run and the numbers are written down in the game's
own notes.

### 0. The game runs, exactly

`gbrecomp` generates the game, and it is played. *Gate:* it boots and plays;
a scripted run ends in the same state twice. *Every game:* all of it.

### 1. The function map, and lifted source

The export, the calls through bank-switching helpers (`edges`), the tables
(`edges --tables`), evidence from play (`--evidence`), and the source folder
(`gb_lift.py source`, `build`).

*Gate:* the checker reports no mismatch; the game built from the folder ends
a scripted run as the recompiled one does; of the time the CPU runs, at least
95% is in lifted functions (`--report-lift`), and `gb_lift.py coverage`
shows no dispatch landing inside a lifted function.

*The game's own work:* the declarations of its tables, which somebody reads
out of the code that uses them, and playing it (or having it play itself,
`gb_lift.py explore`, with a list of the game's checkpoints).

### 2. The data as files

`gb_workspace.py`: the data tree, pictures of tiles, tile maps, strings,
what the game's own unpacker makes, names for the files.

*Gate:* the tree builds to the original ROM, byte for byte; an edited
picture and an edited string show in the game.

*The game's own work:* the charmap, and the declaration of its unpacking
routine (`gb_data.py unpackers` proposes one).

### 3. Names

One names file: functions, variables, data. From a symbol file where the
game has a disassembly; otherwise a model reads the lifted C and names what
it reads, a bank at a time, with the comments it writes kept by `source`.

*Gate:* every function that takes more than a thousandth of the running
time has a name, and every variable those functions use.

*The game's own work:* all of it, once. This is where a game without a
disassembly costs more than one with.

### 3b. Signatures

`gb_lift.py signatures`: what every function takes, gives and keeps, in one
file of the folder (`LIFTING.md`, *Signatures*). It is worked out by a
program, from the original run with one register changed at a time and from
who reads what after each call, and it is what every later step writes calls
from. A person corrects a line where they know better; the tool keeps it.

*Gate:* every function matches held to its line, and the game run with
`--lift-signatures` ends the reference runs as the original does.

Where a function's callers cannot all be listed it gives everything it
changes, until the reference runs of stage 4 have been played with
`--lift-callers`: then it gives what is read where it was seen to return
to, and its line says `[as played]`. So this stage is run twice, before
stage 4 and after it, and again whenever more of the game has been played.

*Every game:* all of it.

### 3c. Data the source names

`gb_lift.py source --data-tree`: an instruction's operand that a played
game read its ROM through is a name in the source, `ctx->hl = BaseStats;`,
and what it leads to can be moved by the data tools, with a header that
tells the source where (`LIFTING.md`, *Data the source names*). A program
does it, from the records of stage 2's tree and the names of stage 3. It is
the first half of stage 7 brought forward: an address that is a name can
later be an array, and one that is a number cannot.

*Gate:* every function still matches; with everything moved that can move
(`gb_data.py build --move-all --moved-header`), the game built with the
header ends the recorded runs on the moved image as the original does on
the ROM, and the game built without it does not.

It is run again whenever more of the game has been played: an operand in
code no record saw run stays a number, and holds what it spells in place.

*Every game:* all of it.

### 3d. The functions, converted

`gb_lift.py convert`: every function that has a signature written again by
a program as a C function with those arguments and that result, its calls
written the same way, and a binding where the lifted function stood
(`LIFTING.md`, *Functions a program converts*). No model, and no reading:
it is what stage 5's writer starts from in place of the lifted function,
and it is where a pass that a program can do is added once and run on
every function. The passes there are still to write are listed in that
section, and each ends at the same gate.

*Gate:* every converted function matches held to its line, and the game
built with them ends the reference runs as the original does. The second is
not a formality: the checker passes C that the game shows to be wrong, since
it delivers no interrupts and throws away every state in which the original
returns past its caller.

*Every game:* all of it.

### 4. Reference runs

Save states along the whole game, and runs recorded from them. Everything
after this is compared against them, so they have to reach the code that is
going to be written: every kind of screen, a battle of each kind, each menu.
`gb_lift.py explore` with a checkpoint list, or a person's recorded run
(`--record-run`) swept for states.

*Gate:* the states exist, and a list says which functions each run enters
(`--trace-entries`), so that for any function there is a run that reaches
it. A function no run reaches is listed as such.

### 5. The functions, written by hand

The pass. One function at a time, leaves of the call graph first: the C
under `src/native/`, a binding where the lifted function stood, `build`.

*Gate, each function:* the checker says `match`, on 100% of its
instructions, with `same time`.

*Gate, each batch:* the game is played against the reference build from
every state of stage 4, with buttons at random, and ends each run with the
same cycle count and the same RAM (the stack and `scratch.txt` aside).

*Gate, the stage:* the share of running time spent in hand-written
functions, counted as lifted share is. At 100% no instruction of the
original runs any more.

The rules a writer keeps, each of which was learnt from a function that
came out wrong without it:

1. **Results are what the callers use; say what the rest is.** A register
   the C has no value for is `lift_undefined`, never a guess. If the game
   then plays differently from the original, the register was a result after
   all: Pokémon Red's `Random` leaves a borrow in the carry flag that the
   next `Random` adds in.
2. **A flag that comes in is an argument.** The same `Random` adds in
   whatever carry its caller had.
3. **Keep the original's time, an instruction at a time**, wherever the
   frame interrupt can land: one `lift_tick` for each instruction of the
   stretch. A count for the whole stretch adds up the same and still lets
   the interrupt in late, and the interrupt reads the hardware's counter
   for the random numbers.
4. **A variable an interrupt handler also uses is touched on its own
   cycle**, with the handler able to run before and after as in the
   original: the mapped bank, the frame flag, the random bytes.
5. **Memory that is a register in all but name goes into `scratch.txt`**,
   with what it is, and is otherwise left as the original leaves it. Where
   keeping a byte faithful costs one line, keep it faithful.
6. **Call what is not written yet through the machine**, with the registers
   that callee reads; call what is written as C.
7. **Leave lifted what cannot be C yet:** a function that reads what it
   pushed, one whose callee returns past it, one whose point is the cycle a
   hardware register is written on. They are listed, and come last.

*The game's own work:* all of the writing. *Every game:* the checker, the
build, the comparison, the rules.

*Not built yet, and wanted before the pass is run at scale:*

- the header of variables written from the names file (a script did it for
  Pokémon Red);
- the converted C (stage 3d) kept up to date in the folder by the tool
  (`convert --write` puts it there once, as functions somebody changed);
- the work list: the functions all of whose callees are written, the ones
  that take the most time first, with the run of stage 4 that reaches each;
- the comparison of stage 5's batch gate as a command of the repository
  (it is a script beside Pokémon Red's folder);
- a way to keep time that does not ask the writer for a number an
  instruction: the instructions a stretch of C stands for, by their
  addresses, with the tool looking the cycles up.

### 6. Without the processor

A function only C calls needs no binding. When none is left, nothing sets a
register or pushes a return address: the main loop is a C function, the
interrupt handlers are C functions the machine calls, and the build drops
the recompiled code and the interpreter.

*Gate:* the game builds without them, and passes stage 5's batch gate.

*Not built:* a build of the runtime as "memory, video, sound and buttons"
with no CPU in it, and the proof for each binding that nothing but C still
enters it (`--trace-entries` gives it for the runs there are).

### 7. Data by name, and no ROM

Until here a table is read at its address in a bank that has to be mapped,
by a name where stage 3c gave it one.
Now every read of the ROM becomes a named array, built from the data tree's
files, and the bank switching goes. A string is text, a picture a PNG, a map
its grid.

Sound is two things. The engine is code, written in stage 5 like any other.
What it plays is data: streams of commands (note, length, octave, tempo,
loop). They are decoded once to a text a person can edit, one file a song,
and put back together by the build. Until that is done the engine written in
C reads the streams as the bytes they are, and the game sounds as it did, so
the decoding holds up editing music and nothing else.

*Gate:* the game builds from the tree alone and passes the batch gate; with
every table and string moved to another place by the build
(`build --move-all` is this test for the data tree today), it still does.

*The game's own work:* the format of its music, and of every table that is
more than bytes (a record's fields). *Every game:* the tree, the build.

### 8. Off the Game Boy

Variables leave their addresses and become structs, video becomes drawing
calls, time becomes frames. From here the game is no longer the original
byte for byte, and what it is compared by changes: the same save file after
the same recorded input, the same frames.

This stage is a port, with a port's freedom, and is the first where a
choice changes what the player gets. It is not planned further here.

## Time

"Match the timing" means three different things, and they are met in order.

1. **How long each function takes.** The C says, in cycles, and the
   checker's `same time` proves the count on every path it tried. This is
   what keeps the game's pace: the frames a map takes to load, the lag
   where the original lags.
2. **Where an interrupt lands.** One `lift_tick` an instruction. This is
   what keeps the random numbers, and with them whole runs, the same as the
   original's, which is what makes stage 5's batch gate a comparison of
   bytes and not of impressions.
3. **The cycle a hardware register is written on**, for the few routines
   that draw by it (a split screen, a wave down the picture). They stay
   lifted, where every access is on its cycle already, until last.

After stage 8 only the first is left, as frames.

## What a game needs that Pokémon Red had

| | Pokémon Red | A game without it |
|---|---|---|
| Names | A community disassembly's symbol file. | Stage 3 is a model's reading pass, and the gate is the same. |
| Table declarations | Written with the disassembly open. | Read out of the lifted C of the routine that uses each table. |
| Reference runs | 204 save states to the fourth badge, from the explorer with a checkpoint list. | The same, with that game's list; or a person's recorded run. |
| An unpacker | One routine, declared. | `gb_data.py unpackers` proposes; some games have several, some none. |

Things that would stop a game at a stage, and what is known of each:

- **Code compiled from C** (a game made with GBDK) keeps its locals on the
  stack and reaches them through `ld hl, sp+n`. The checker throws such
  states away for hand-written code, so stage 5 has no verdict for those
  functions as it stands. Nothing has been tried.
- **Code the game writes into RAM and runs.** Not lifted, so not written.
- **A game whose function map is poor.** Stage 1's gate says so before any
  writing is spent on it.

## Other consoles

Stages 0 and 2 have a counterpart on every console the project runs. Stages
1 and 5 rest on three things only the Game Boy has: an export of every
function, a checker that runs a function's C against the original from a
machine state, and lifted source the game is built from. A console gets this
plan when it has those three, and they are the larger part of the work on a
machine with a bigger processor: an ARM or a MIPS function has a calling
convention, which makes signatures easier than here, and far more code,
which makes the map harder.

## Where Pokémon Red stands

| Stage | State |
|---|---|
| 0 | Done. |
| 1 | Done: 6,243 functions lifted, 4,445 of them checked and built in, none differing; 97.9% of running time in lifted code. |
| 2 | Done for pictures, tile maps, text and the packed pictures (all 355 streams). Music and sound effects are still bytes. |
| 3 | Done from the disassembly's symbol file; the home bank is commented. |
| 3b | Done: 3,689 functions have a signature, 2,220 of the 2,470 that start at a label; all match held to it, and the game held to them ends 626 of 626 runs as the original does. What they take and keep is settled. What they give is exact for 622, as played for 283, and everything they change for the other 2,784, which the play there is did not reach. |
| 3c | Done as far as the play goes: 352 operands go by 348 names, 320 of them the disassembly's labels. 700 of the 1,151 things the records reached in data can move, where 497 could. With 325 of them moved (47,734 bytes) the game ends 410 of 410 recorded runs as the original does, and 407 of 410 with other buttons. 182 things are still held by operands in code no record saw run. |
| 3d | 3,277 of 6,243 functions converted by a program, all matching held to their lines: 3,004 as C with saved registers gone and 2,161 calls from C to C, 273 still on the machine's stack. Written into a copy of the folder, the game ends 626 of 626 runs as the original does. 1,791 of the C functions can only be called by the machine, because they leave by a jump into code the export cannot say comes back; far calls are still calls of the helper; nothing is renamed. |
| 4 | Partly: 204 states to the fourth badge. The rest of the game has not been reached, and no list yet says which run enters which function. |
| 5 | Nine functions of about 2,300. The method is proven on them; the tools listed under stage 5 are not built. |
| 6, 7, 8 | Not started. |
