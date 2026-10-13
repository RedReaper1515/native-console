# Lifting Game Boy code to source

Recompiled C is the original program one instruction at a time. It runs, but
nobody can read or edit it. Lifting turns a function into ordinary C that says
what the function does, and keeps a machine check between the two so that an
edit which changes behaviour is caught.

The aim is source for a whole game that can be edited and rebuilt into a
playable executable. Three parts of that exist, for any Game Boy or Game Boy
Color ROM:

- **the export**: everything the recompiler knows about a ROM, as one JSON
  file meant to be handed to a person or a model, a function at a time;
- **the checker**: runs a function's C against the original code from many
  machine states and reports whether they did the same thing;
- **the build**: `gbrecomp --lift` puts lifted C into the generated game in
  place of the subroutines it stands for, so that editing the C and rebuilding
  changes the game.

What is not built yet is listed under [Not done](#not-done). The main gap now
is the last step of readability: the tool writes structured C with names, but
registers are still registers. A function written by hand as ordinary C takes
that step, and the checker holds it to the ROM like any other
([Functions written by hand](#functions-written-by-hand)).

## Working in the source

The sections after this one are the parts, one at a time. This is the way to
use them together: a folder of the game's C that is yours to keep, edit and
build, made by one command and built by another.

```bash
# Once. The export is step 1 of "A first run"; after `--` go the analysis
# options it was made with, which the game is generated with too.
python3 tools/gb_lift.py source output/lift/game.json -o output/game-src --rom game.gb \
    --symbols game.sym -- --pointer-scan strict --lift-edges output/lift/edges.json

# Every time: check what changed against the ROM, generate, build.
python3 tools/gb_lift.py build output/game-src
./output/game-src/build/x64-release/game/game      # build/release/game/game off Windows
```

| In the folder | What it is | Whose |
|---|---|---|
| `src/lifted_bank_NN.c` | The structured C ([What the tool writes](#what-the-tool-writes)), a file a bank. | Yours, once written. |
| `src/lifted_names.h` | Names for fixed addresses. | The tool's: written again every time. |
| `src/lifted_data.h` | Names for what instructions' operands lead to in the ROM, with `--data-tree` ([Data the source names](#data-the-source-names)). | The tool's: written again every time. |
| `src/lifted_moved.h` | Where those names lead in the image the game is to run. Nothing in it until something has been moved. | `gb_data.py build --moved-header`'s. |
| any other `.c` under `src/` | C of yours that the functions call: a file with no lifted function in it ([Functions written by hand](#functions-written-by-hand)). | Yours. |
| `names.sym` | Names of your own, `BB:AAAA Name` a line, as an RGBDS `.sym` file has them. `--symbols` adds a file's names where this one has none. | Yours. |
| `changes.txt` | The functions you changed on purpose, `BB:AAAA why` a line. | Yours. |
| `scratch.txt` | RAM that a function written by hand need not leave as the ROM's code does, `AAAA why` a line ([Functions written by hand](#functions-written-by-hand)). | Yours. |
| `signatures.txt` | What each function takes, gives and keeps, a line a function ([Signatures](#signatures)). | The tool's to begin with; a line you change is yours. |
| `lift-source.json` | The record: for each function a hash of the C the tool wrote, a hash of the instructions it was written from, and what the checker last said of which text. Also the ROM and the analysis options. | The tool's. |
| `CMakeLists.txt`, `CMakePresets.json` | The game as a CMake project with `src/*.c` in it. | Written once, then yours. |
| `include/gblift.h` | What the source includes, for an editor, until `game/` is there. | The tool's. |
| `game/` | The project `gbrecomp --lift` generates. | The tool's: made again when it has to be. |
| `unplaced/` | Changed functions an update found no place for. Not compiled. | Yours to go through. |
| `.lift/` | The export, the package, the checker's work, and `src/` as it was before the last update. | The tool's. |

**What counts as changed.** The unit is one function, known by its
`GBLIFT_FUNCTION(bank, address, name)`: that is what the compiler and the
checker go by, and the `gbfn:` line above it is a comment like any other. A
function is everything from the line after the function before it ends, down
to its own closing brace. So a comment you write above a function is that
function's, and so is a helper or a `#define` you put there. A function is
changed when that text is not the text the tool wrote, a comment included;
spaces at the ends of lines and the kind of line ending do not count. What is
above the first function, down to the last `#include`, belongs to the file.

A changed function is one of two things. Where all you did was add lines of
comment, and blank lines, between the lines the tool wrote, it is **only
commented**: the code is still the tool's. Anything else is a change to the
**code**, and that takes in more than statements: a comment put at the end
of a statement's line, a name changed inside `GBLIFT_FUNCTION(...)`, a
statement moved or taken out, a line of the tool's own comment edited or
deleted. The tool does not guess which it is. It takes the lines that are
nothing but comment out of the function, leaving its own (the `gbfn:` line,
the line that begins `takes`, an instruction that left no statement), and
what is left has to be, by its hash, the text the record says it wrote.

**Running `source` again**, with the same export or a new one (the analysis
told more, a name added to `names.sym`, another `--time`), brings the folder
up to date:

- a function whose code you changed is **kept**, exactly as it is;
- a function you only commented is **carried**: written again from the
  export like one nobody touched, with your comments put back where they
  stood (below). Where the tool would write what it wrote before, the
  function is not rewritten at all;
- a function nobody touched is **refreshed**: written again from the export,
  with the names the folder now has;
- a function the folder did not have is put in where its address belongs, and
  an untouched one that is no longer a function is dropped;
- a function you took out stays out. `--restore BB:AAAA` brings back the
  tool's version of one, taken out or changed.

**Comments that carry.** Write a comment on lines of its own, and leave
the tool's lines as they are: then the function stays the tool's to write,
and takes every name given later, a change of `--time`, and whatever the
tool learns to write better, with your comments in it.

- Above the function, anywhere over `GBLIFT_FUNCTION`: over the tool's two
  lines, between them, or under them. It goes back to the same place.
- Inside, over the statement it is about. Every statement the tool writes
  ends with the address of its instruction, `/* 0061: xor a */`, in both
  ways of keeping time, and that address is what the comment goes by: it is
  put over the statement that instruction has in the new text. A comment
  over a `do {` or a label goes by the first instruction under it, and is
  put over the `do {` or the label that is there now; one under the last
  statement stays at the end. It is moved in or out as far as its statement
  is.
- A blank line counts as a comment, and goes with the line under it.
- A name goes into `names.sym`, not into `GBLIFT_FUNCTION(...)`: there it
  reaches the callers' comments too, and the function is not held back.
- Do not begin a comment line of your own with four hexadecimal digits and
  a colon: that is how the tool's own begin, and the function is then taken
  for changed code and kept as it is.

After the comments are put back the result is taken apart again and the
added lines counted: none may be missing. Where a comment is about an
instruction the function no longer has, because the export cuts the function
shorter, nothing is moved to a nearby place: the function is left exactly as
it is, comments and old code, and named, each time, until the instruction
is back or you move the comment.

What it **could not reconcile** it names, and loses nothing. A changed
function that is no longer a function of the export cannot stay in `src/`
(nothing would call it): it is taken out and its text put in `unplaced/`. A
function whose code you changed, and whose instructions are no longer the
ones it was written from, is kept where it is and named; `build` checks it
again. It can still match, since the checker follows the code from the
function's first instruction wherever the export now cuts it: of 200 such
functions of Pokémon Red none mismatched
(see [What it does on real games](#what-it-does-on-real-games)). A function
you only commented, with a comment about an instruction it no longer has, is
kept and named in the same way. The tool does not merge code: a function
whose code you changed does not get the new names, nor anything else the
tool would now write differently. `lifted_names.h` changed by hand is put in
`unplaced/` and written again with its names kept; a name belongs in
`names.sym`.

**`build`** does four things and stops at the first that fails.

1. *Check.* Every function whose text is not the text the checker last gave
   a verdict on goes through [the checker](#the-checker): all of them the
   first time, afterwards the ones you edited. Images of the game's memory
   (`gb_lift.py image`) put in `images/` are checked from as well, and so
   are the states a played game entered its functions in (`--entry-states`,
   see [The states](#the-states)) put in `entries/`.
2. *Decide.* A function that matches is built in. One the checker could not
   run is left out, as `package --report` leaves it out, and the game runs
   the ROM's code there; `build` says so if it is one you changed. A function
   that no longer does what the ROM's code does stops the build, with nothing
   built, unless `changes.txt` names it: then it is the mod, and it is built
   in whatever the checker says. A function only commented or tidied matches
   and needs no line.
3. *Generate.* `gbrecomp --lift` writes `game/`, only when the list of
   functions built in, the analysis options or the recompiler changed.
4. *Compile*, with `cmake --preset` and `cmake --build --preset`.

**What is edited is what runs.** The game compiles `src/*.c` where they are.
The package handed to `gbrecomp` holds a stub in place of the sources, so
`game/lift/` has no copy of them, and the folder's `CMakeLists.txt` adds
`src/*.c` to the generated `game` target. A copy would have been simpler to
make and worse to work with: a compiler error, a breakpoint and
go-to-definition would open the copy, and an edit made there is lost. It
also means an editor's own build runs your edit without the tool; only the
check is skipped.

**Time.** The folder is written with `--time exact` unless told `--time
block`, so that the game built from it untouched ends in the same state as
the original (flags nothing reads aside), and a difference seen later is the
edit's. `source EXPORT -o FOLDER --time block` switches a folder there is,
and `--time exact` switches it back: the functions nobody touched and the
ones only commented are written the other way, comments in place, and the
ones whose code you changed stay as they are, which a game does not mind.
`build` checks every function that was written again.

**Visual Studio.** *File > Open > Folder* on the folder. `CMakePresets.json`
is after the one Visual Studio writes for a new CMake project: Ninja, the
64-bit MSVC tools, `x64-release` and `x64-debug`, with the SDL2 found when
the folder was made. `gb_lift.py build` configures and builds with the same
preset from the same folder, so the two share `build/`. Before `game/`
exists the folder still configures: the functions are compiled on their own
against `include/gblift.h`. What has been run is the command line, with the
CMake and Ninja that ship with Visual Studio's build tools; the IDE itself
has not been opened on a folder (see [Not done](#not-done)).

## Functions written by hand

What the tool writes is the original's instructions with registers. The step
after it is the function as C would have it: arguments and results, the game's
variables by name, in a file where it belongs. This is the way from a lifted
game to a game written in C, a function at a time, with the game playable and
checked the whole way.

```c
/* src/native/home/copy.c */
#include "../game.h"

/* Copies `count` bytes from `from` to `to`, lowest address first. */
void CopyData(uint16_t from, uint16_t to, uint16_t count) {
    do {
        lift_write8(gb, to++, lift_read8(gb, from++));
    } while (--count);
}
```

```c
/* src/lifted_bank_00.c, where the lifted function stood */
/* gbfn:v1:0000:00b5  CopyData  (8 instructions) */
GBLIFT_FUNCTION(0x00, 0x00B5, CopyData_binding) {
    const uint16_t count = ctx->bc;
    gb = ctx;
    CopyData(ctx->hl, ctx->de, count);
    ctx->hl = (uint16_t)(ctx->hl + count);
    ctx->de = (uint16_t)(ctx->de + count);
    ctx->bc = 0;
    ctx->a = 0;
    ctx->f_z = 1; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = 0;
    lift_ret(ctx);
}
```

The second is a **binding**: the function's lifted text, changed into the few
lines that hand the registers to the C and put back what the original leaves
in them. It keeps the function's place in its bank's file, so `source` keeps
it as it keeps any function whose code somebody changed, the game enters it
wherever the original is called from, and `build` checks it against the ROM
like any other. It has another name than the C function, since both are C
functions.

**Where the C goes.** Any `.c` file of `src/` that has no `GBLIFT_FUNCTION` in
it, and any `.c` file of a folder under `src/`, which may have none: a
function of the ROM's has its place in a bank's file, where an update of the
folder looks for it. `build` compiles all of it into the checker and the game
from where it is, and `src/` is where its `#include` lines are looked up, as
well as beside the file. `source` does not touch it. A folder made before
this needs two lines of its `CMakeLists.txt` changed, which is the folder's
own: `GLOB` to `GLOB_RECURSE` for `src/*.c` and `src/*.h`, and
`target_include_directories(game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")`.

**What it is written against.** `gblift.h`, as lifted functions are, with two
things for this:

| | |
|---|---|
| `lift_ram(ctx, address)` | Where a byte of work RAM or high RAM is. `#define wCurMap (*lift_ram(gb, 0xD35E))` makes a variable of it: `wCurMap = 5;`, `&wMonHeader`. The rest of the bus still goes through `lift_read8` and `lift_write8`, by address. |
| `lift_undefined(ctx, registers)` | The registers and flags the function leaves with nothing in them worth having: the last byte a loop looked at, the flags of its last compare. The checker does not compare them. The game gives them a fixed value that is nobody's, so that code which did read one goes wrong the same way every time, and a run compared with the original shows it. |

Code that is not written by hand yet is called through the machine:
`lift_call(gb, address, return_address)` with the registers the callee reads
set in `gb`, and `lift_halt(gb)` waits for an interrupt, so a function that
waits for the next frame can be C. One function written by hand calls another
as C calls C: no return address is pushed, and none is needed. Where a callee
may drop its return address and go back to the caller's caller, `lift_call`
says so and everything since the binding was entered has to be left at once,
which takes a `setjmp` in the binding.

**What such a function is let off.** Three things, each by what the function
itself does or the folder says, and none of them for a function the tool
wrote.

- *Registers it called undefined*, in the state it called them so.
- *The RAM `scratch.txt` lists*, `AAAA why` or `AAAA-BBBB why` a line: bytes
  that are registers in all but name. A helper parks `A` in one for a
  moment; a far call keeps its caller's registers in six. Leaving them as
  the original does would mean handing register values through every
  signature for the sake of bytes nothing reads. They are left out for a
  function whose code somebody changed and for one held to a
  [signature](#signatures), and compared as ever for any other function
  the tool wrote, comments added or not. Nothing checks that
  nothing reads them: the evidence is the game played against the original
  (below).
- *States in which the original uses its own stack as data*
  (`used_its_stack_as_data` among the states not usable): a copy whose
  made-up pointers take in what the function itself pushed. Where the two
  sides then differ, and the source did not push what the original pushed,
  the state is thrown away and not called a mismatch: C that pushes nothing
  has no such bytes to agree about, and a game does not do that. Source
  that keeps the original's stack, as everything the tool writes does, is
  held to such a state like any other. A function with more states thrown
  away for this than states that agreed gets no verdict: the ones that
  agreed may be the ones where what was on the stack happened not to
  matter. The same goes for the time. A search that runs on over what the
  function pushed can end as the C ends and take another time over it,
  having gone the longer way round one of those bytes: the state counts as
  agreeing, and its time is neither the same nor different but not
  compared (`not_compared` under `cycles` in the JSON). So a function that
  reads what it pushed on purpose
  (`ld hl, sp+0`), written as C that does not, is `inconclusive`, and
  `build` says it is not in the game.

**An edit to the C** changes no binding's text, so `build` keeps one hash of
all of it, headers and `scratch.txt` included: when that changes, every
function whose code somebody changed is checked again, and no other.

**Time.** C that says nothing of time runs in none: the checker reports
`time not counted` and the game plays, since a game waits for its frames.
C that is to keep the original's pace says how long each stretch takes, with
`lift_tick`, and the checker's `same time` is the proof that it adds up on
every path. Where the frame interrupt samples a free-running counter, as a
game's random numbers often do, the count has to be one `lift_tick` an
instruction and not one for a stretch of them: the interrupt is then taken
between the same two instructions as in the original. A variable an interrupt
handler also uses is read and written on its instruction's own cycle
(`lift_advance` before the access, as the tool's own text does).

**On Pokémon Red** nine functions were written this way
(`CopyData`, `FarCopyData`, `FillMemory`, `AddNTimes`, `DelayFrame`,
`DelayFrames`, `Random`, `Random_`, `GetMonHeader`, the last of which calls
three of the others and one function that is still the ROM's). All nine match
in every usable state, `same time` in all, 100% of their instructions
reached, and a check after an edit takes five seconds. The game with them was
played against the game without, from a fresh boot and 26 save states along
the story, four times each with other buttons at random, 3,000 frames a time:
the 108 runs end with the same count of cycles and every byte of work RAM,
high RAM and cartridge RAM the same, apart from the stack and two stretches
of scratch. Three things had to be found on the way, and each is the reason
for something above:

- With time counted a stretch at a time, a third of the runs ended with
  other random numbers: the frame interrupt read the hardware's counter up
  to a stretch late.
- `Random`'s flags were called undefined, and the player's ID number came
  out one higher. The game calls `Random` twice with nothing between that
  changes the carry flag, and the second call adds in the borrow the first
  left: the flag is a result, and the C now has it as one. The fixed value
  the game puts in an undefined register is what showed it.
- Runs that stop inside a waiting function differ in the stack pointer and
  in the byte where the frame interrupt keeps it: C calls push nothing.

**Not done for this.** The header of variables is not written by the tool,
and neither is a binding: both were made by small scripts for the one game.
A save state made inside such a function resumes in the ROM's code at
whatever `ctx->pc` holds (see [Save states](#save-states)). Nothing proves a
byte of `scratch.txt` is never read before it is written. And a run compared
with the original says the two agree where it went, no more.

## Signatures

Assembly has no signatures. A function reads whatever registers it reads and
leaves something in every one of them, and which of that is an argument, a
result, a register saved and restored, or what happened to be left over is
written down nowhere. Source with arguments needs it written down, for every
function, before the first one is rewritten: it is what a call becomes.

```bash
python3 tools/gb_lift.py signatures output/game-src
```

writes `signatures.txt` in the folder, a line a function:

```
00:36E0  takes a bc hl  gives -  keeps de        ; FillMemory
00:3E5C  takes fc  gives a fc  keeps bc de hl    ; Random
```

Registers are `a b c d e h l` (`bc`, `de`, `hl` for both halves), flags
`fz fn fh fc` (`flags` for all four), `-` for none. Whatever a line neither
gives nor keeps, the function leaves **undefined**.

| | What it means | Where it comes from |
|---|---|---|
| `takes` | The registers and flags it reads. | The original itself: `check --observe` runs the function from each state once more for every register and flag, with another value in it. It is read if anything else then comes out differently: a byte of memory, another register, what the hardware was told, whether it returns, or how long the function took. One that is only pushed and popped comes out as it went in whatever it held, and is not read. |
| `keeps` | The ones that come out as they went in. | The same runs: from every state tried. |
| `gives` | Of what it changes, what something reads afterwards. | The export: what is read after every call of it, after the helper a far call goes through has finished, and after any function that jumps or runs on into it (`read_after_return` in `tools/gb_lift_signatures.py`). Where the function has a way in whose users nobody can list (a table nobody declared, an address in data, an interrupt), that is everything it changes, until a played game says who they were ([below](#who-calls-what-from-the-game-played)). |

The first two are evidence from the states there were, as a match is. The
third is as good as the export's list of callers. So a signature is not
believed: it is **held**, three ways.

- **By the checker**, in every `build`. A function with a line has every
  register and flag it does not take given another value before its source
  runs, what it keeps put back as it came afterwards, and what it leaves
  undefined not compared. A line that leaves out a register the function
  reads, or says it keeps what it changes, is a mismatch, and stops the
  build like any other. A function is checked again when its line changes.
  It is not held to its line from a state in which the original used what
  it pushed as data: a fill that runs over its own saved registers does not
  keep them, and the two are compared there as they are.
  The RAM `scratch.txt` lists is left out for a function held to a line, as
  it is for one written by hand: a helper that parks its caller's registers
  there parks the other values, and a register that only ever reaches
  scratch is not one the function takes.
- **By `signatures` itself**, which runs that check before it is done. A
  function that matched without its line and does not with it, or that
  matches with it in another time than the original's where without it the
  time was the same, is tried on
  the next rung down, one a round: watched again, from every state the
  check uses and not the first 160 of each kind; then taking, besides what
  it was seen to read, everything the export says it may read (a register
  is read on a path too rare to watch, and the export counts a register
  that is only saved as read); then the line the export alone gives, which
  errs on the side of taking and giving; then no line. It says how many
  functions ended on which rung.
- **By the game**, for what the checker cannot see. Whether a caller reads
  what a line calls left over is not something a function compared on its
  own can show. `--lift-signatures FILE` (the file is `.lift/signatures.masks`,
  written by `signatures` and by `build`) has the game put back what each
  lifted function keeps, and put a value that is nobody's into what it
  leaves undefined, each time one returns to its caller. A game whose lines
  are right plays as it does without the option. Where a line calls a
  result left over, the caller gets the other value, and a run compared
  with the original shows it. This is a way to test, not a way to play: the
  registers are not what the original leaves, so a run cannot be recorded
  or replayed with it.

**What only the time depends on is taken too.** A conditional jump to the
instruction after it takes a cycle longer when it is taken and leaves
everything as it was either way; data read as code is full of them, and
real code has a branch now and then whose two sides end alike. The flag
such a jump asks about is one the function takes. It has to be: a function
held to its line is given another value for whatever the line leaves out,
and how long it took is compared with the original, so a line without the
flag makes the function `match`, `time differs`, for a branch the source
has exactly as the ROM has it. So the watching counts a register or flag as
read where the original, run again with another value in it, came out the
same and took another time. Where the watching had no state that showed
it (160 of each kind, and the branch may be one a state in a thousand
reaches), the check has: `build` writes into the folder's record what it
found of each function's time (`clock` in `lift-source.json`: `same`,
`differs`, or `not counted` where no state had a time to compare), and
`signatures` takes a function that
`differs` held to its line, and did not without one, down the rungs like
one that does not match.

### Who calls what, from the game played

Most functions of a banked game have a way in the export cannot list the
users of: its address stands in data somewhere, or a table names it. Most of
those addresses stand there by chance and nobody ever comes that way, but
nothing in the ROM says so, and such a function gives everything it changes.
A played game does say who came:

```bash
./game --lift-callers logs/run1.callers        # play; the file is written when the game stops
mkdir -p output/game-src/callers && cp logs/*.callers output/game-src/callers/
python3 tools/gb_lift.py signatures output/game-src
```

`--lift-callers FILE` notes, each time a lifted function is back with its
caller, the address it returned to and the ROM bank that address is in.
Where lifted code had jumped to the function as a call (a bank-switching
helper does), it notes the helper and where the helper was to return as
well, so that what is read afterwards is what the helper's own caller reads.
It changes nothing in the game. The line format is in `gbrt_lift.h`.

`signatures` reads every file in the folder's `callers/` (and any given with
`--callers`). A function with an unlisted way in that was seen to return
somewhere gives what is read after the places it was seen to return to and
after the calls the export lists, and no longer everything; its line ends
`[as played]`. Three things keep that honest:

- A function **never reached** in play is left as it was: no run says
  anything about who calls it.
- A place **nothing accounts for** (not after a call, not where a helper's
  jump comes back: the middle of whatever an interrupt came in on) makes the
  function give everything again.
- A caller a run saw that the export **does not list** is counted for a
  function with every way in listed, too: there the play can only widen.

A helper that pushes the address to come back to and jumps through a
register is followed both ways the export can have it: kept whole, with the
place it comes back to among its own blocks, or cut in two there, because a
run saw something return to that address and the export made a function of
it. Then that function is the rest of the helper, and what is read after a
function called through the helper is worked out through it all the same.

What `[as played]` claims is no more than it says: nothing that called the
function in the recorded play reads more. A caller that play never reached
may, and the only test of that is more play: the game held to the
signatures (`--lift-signatures`) and compared with the original, on runs
that were not the ones recorded.

On Pokémon Red the game was played from a fresh boot, from 204 save states
along the story four times each for 3,000 frames, and from 19,504 states an
exploration had kept for 900 frames each, with buttons at random: 20,325
runs, in which 752 lifted functions were seen to return somewhere. With
that, 283 functions that have an unlisted way in give what their callers
read, and the helper rule made 71 more of the listed ones exact; 224
functions give less than they did, 727 leave something undefined where 530
did, and `IndexToPokedex`, which was `gives a flags`, gives nothing: its
answer is in memory. The game held to the new lines ends 626 of 626 runs as
the original does: the 108 as before, 108 with other buttons, and 410 from
every one of the save states with other buttons again, none of them runs
that were recorded. What it did not change is the larger part: 2,784 of the
3,689 functions still give everything they change, because they have a way
in nobody can list and were not reached in that play. Twenty thousand runs
of random buttons from the same places keep finding the same code; a game
played through by somebody would reach the rest.

**The file is yours.** A line the tool wrote is written again when the tool
finds out more; a line somebody changed is kept as it is, and `build` holds
the function to it; a line taken out stays out. `--restore BB:AAAA` writes
the tool's line again. A function the checker cannot run has no line, and
neither has one that never returns.

**On Pokémon Red** the tool wrote a line for 3,687 of the 4,445 functions
the checker can run, and for 2,220 of the 2,470 of those that start at a
label of the disassembly's symbol file. All 3,687 match held to their line,
and every other function matches as before. Of the 2,220: 1,031 take no
register at all, 267 take a flag, 1,080 keep at least one register, and 485
leave something undefined. 194 functions did not hold as first watched: 65
of them held once watched from every state, 105 took what the export says
they may read as well, 20 have the export's own line, and 4 have none. From
the export alone only 539 functions had every way in listed, so the rest
gave everything they change: `CopyData` is
`takes bc de hl  gives a bc de hl flags`, where a person would say it gives
nothing ([the game played](#who-calls-what-from-the-game-played) narrows
some of that). The game held to all of it was played
against the game without, from a fresh boot and 26 save states, four times
each with other buttons at random, 3,000 frames a time: 108 of 108 runs end
with the same count of cycles and the same work RAM, high RAM and cartridge
RAM, the stack and `scratch.txt` aside. With `Random`'s line changed to call
its carry left over, the run from a fresh boot ends with another player ID.

Three things were wrong on the way, and each is a rule now. A register that
changes on one path and goes through untouched on another has to be taken as
well as given, or the other value it was handed comes out as a result. What
`scratch.txt` lists has to be left out when a function is held to its line
as it is when it is watched: `Predef` parks its caller's registers there,
and nearly 400 functions did not hold at first until this was so. And a
function stopped at a wait has what it pushed still on its stack, in the
other values.

A fourth showed later, and in the time alone. Held to their lines, twelve
of the functions matched and took another time than the original: three in
every one of 2,000 states, which are data read as code (a conditional `jr`
to the instruction after it), `Route8CooltrainerF4BattleText`, text read as
code, in 542, and eight in one to eight states, `CanWalkOntoTile` and
`LoadFrontSpriteByMonIndex` among them. Each has a register or a flag that
decides how long it takes and nothing else, which its line left out. With
the time counted where a function is watched, nine of the twelve lines take
it as first watched. The other three take another time only in a state
where they stop at a wait, where nothing is watched: they went down the
rungs for it, and take what the export says they may read. Twelve lines of
3,891 changed, each to take more and in nothing else, and held to them no
function takes another time than the original.

**What a line does not say.** What a function reads and writes in memory,
which the export lists. Which value a register holds when it is "kept" on
one path and a result on another: such a function gives it. And nothing
about a caller the export does not have: code in RAM, a way in through a
table nobody declared. Those make a function give everything it changes,
which is safe and tells a rewrite less than there is to know.

**Not done for this.** A function is not given other values for what it does not take
when the game runs, only in the checker: in the game that would leave a save
state made inside a function holding them. And a line says nothing of
memory: which variables a function reads and writes is in the export, and is
not part of what it is held to. And a function is not watched from a state
in which it stops at a wait, though it is held to its line there: what
decides only how long it takes to reach the wait is found when the
function, held, takes another time, and then from what the export says it
may read, which counts a register that is only saved.

## Data the source names

`ld hl, $43DE` comes out of the tool as `ctx->hl = 0x43DE;`. The number is
an address in the ROM, of a table, and as long as the source has it as a
number the table has to stay at $43DE: nothing can tell the instruction that
it went somewhere else. The data tools say so of every thing an operand
leads to (`DATA_FILES.md`, *Data that grows, by moving*), and on a real game
that is the largest reason data cannot move.

With a name in its place the table can go anywhere the name is told about:

```bash
# A data tree of the ROM, with a played game's records of its pointers in it.
python3 tools/gb_data.py pointers output/data/game output/data/records

# The source again, with what those records saw operands lead to.
python3 tools/gb_lift.py source output/lift/game.json -o output/game-src --data-tree output/data/game
python3 tools/gb_lift.py build output/game-src

# The tree again, told which operands the game now has by name.
python3 tools/gb_data.py pointers output/data/game output/data/records \
    --named output/game-src/.lift/named-operands.json
```

**What gets a name.** An operand a record saw the game read its ROM through,
of `ld bc, nn`, `ld de, nn`, `ld hl, nn` or `ld a, [nn]`, in a function of
the export. Nothing else: two bytes that happen to say an address are a number
until a played game has shown them to be one, and the same two bytes in a
function no record saw run stay a number. The name is the label `names.sym`
or `--symbols` has at that very address, where C can have it and nothing
else has taken it, and `rom_BB_AAAA` otherwise. An operand that led into two
banks has no one thing to be named after and stays a number.

**The two headers.** `src/lifted_data.h` defines each name as the address
the ROM has the thing at, inside `#ifndef`, after it has included
`src/lifted_moved.h`. That second file is empty until `gb_data.py build
--moved-header` moves something: then it defines the names that moved, with
where they are in the image it built, and those win. `lifted_names.h`
includes `lifted_data.h`, so every source file has both. The checker is
compiled with `GBLIFT_CHECKING`, which leaves `lifted_moved.h` out: it holds
the source to the ROM's code with the ROM as it is, and that is still a
proof of every function, since the two differ by a number the build puts in.

**What `build` adds.** `.lift/named-operands.json`
(`gbrecomp.lift-named-operands`, version 1), which is what `pointers
--named` takes:

- `operands`: each operand the game really has by name, as the ROM `offset`
  of its low byte, the `name`, and `size`, how far the thing runs for whoever
  wrote the names: to the next label in its bank. Only operands of functions
  that are built in and whose code is the tool's. A function somebody
  changed may have the number written out, and a function the game runs the
  ROM's code for has the ROM's operand.
- `unnamed`: every other operand of the export that spells an address below
  $8000, with the `bank` its instruction is in and the `value` it says.

**What can move then.** A named operand is a pointer like one that lies in
data, with one difference: it is turned in the source, not in the image.
Four things still keep a thing where it is, and the report counts each:

- the game also compared the operand, reckoned with it as a number, or
  jumped to what it made. The recorder marks an operand for that as it marks
  a byte of data. The last of them is a string with code behind it: the game
  reads along the string and then runs on from where it stopped, and code
  cannot move;
- the game reads the operand's own bytes as data, as it does of code it
  copies into RAM and runs there. The source turns the instruction, and the
  copy keeps the address the ROM has;
- an operand no source names spells an address the thing's pointers lead to
  (`spelled`). No record saw that operand read the thing, and it would go on
  reading the old place;
- anything that kept it before: a read through an address nobody could
  follow, a pointer in a file of code.

A thing an operand leads to is copied as far as its name runs, to the next
label, where that is further than the records read: a table is read by
counting from its start, and no record of a game says how long it is.

**Building with something moved.** `gb_data.py build --moved-header
output/game-src/src/lifted_moved.h` writes the image and the header
together, and `gb_lift.py build` then compiles the game for that image. The
game and the image belong together from there on: a game built with the
header reads the new places, which in any other image hold something else.
`build` refuses to move a thing the source names without being told where
to write the header, and writes the header again on every build that names
it, with nothing in it when nothing the source names has moved.

`generated_lift_named_data_end_to_end` is the test: five tables read through
`ld hl`, of which one moves; one stays because a function that never ran
spells its address too, one because the game compares the operand, one
because the game jumps on from it, and one because the game runs a copy of
the function that reads it from RAM. The game built with the header ends on
the moved image as the original does, and the game built without it reads
the old place, which that image has spoilt.

**On Pokémon Red**, with records of 410 runs from the save states along the
story: the source names 352 operands by 348 names, 320 of them labels of the
disassembly (`BaseStats`, `PokedexOrder`), and every function still matches.
Of the 1,151 things the records reached in data files, 700 can move, where
497 could before. `build --move-all` moved 325 of them, 47,734 bytes, turned
441 pointers in data and sent 110 names somewhere else. The game built with
that header ends each of the 410 recorded runs on the moved image as the
original does on the ROM, with the old places spoilt. With other buttons
from the same states, 407 of 410: three runs came to data no record had seen
read. The game built without the header ends 1 of the 410 the same on that
image, which is what says the header is doing the work.

**What this is not.**

- **Not yet a ROM without addresses.** 182 things are still held by operands
  the source has as numbers, in functions the records did not see run, and
  175 by reads nobody could follow. More play names more.
- **The proof is the runs.** A thing that moved is right for every path a
  recorded run took. A path that reads it through an operand nobody saw at
  work reads the old place: the `spelled` rule keeps the things such an
  operand names outright, not the ones it reaches by adding to another
  address.
- **Within a bank.** The name stands for two bytes of address. The bank is
  still mapped by the code around it.
- **The game does not check the image against the header.** A game built
  for one image and given another plays wrong and says nothing.
  `LIFTED_MOVED_IMAGE` in the header is the image's hash, for a check that
  is not written.
- **RAM is not part of this.** `lifted_names.h` has had names for fixed
  addresses of RAM and hardware since the folder was first written.

## Functions a program converts

A lifted function takes a context, and that is all its text says of what it
is given. `signatures.txt` says the rest. `convert` writes each function
again as what its line describes: a C function with the registers it takes
for arguments and the ones it gives for its result.

```bash
python3 tools/gb_lift.py convert output/game-src            # writes .lift/convert/ and checks it
python3 tools/gb_lift.py convert output/game-src -o out --function 00:3A87
python3 tools/gb_lift.py convert output/game-src --write    # and puts what matches into src/
```

```c
/* gbfn:v1:0010:4474  DrawTileLine  (9 instructions) */
/* takes bc de hl  gives hl flags  keeps a bc de */
DrawTileLine_r DrawTileLine(GBContext* ctx, uint16_t bc, uint16_t de, uint16_t hl) {
    LiftRegs r = {0};
    r.bc = bc; r.de = de; r.hl = hl;
    lift_step(ctx, 0x4475, 16);  /* 4474: push bc */
    lift_step(ctx, 0x4476, 16);  /* 4475: push de */
    do {
        lift_advance(ctx, 7); lift_write8(ctx, r.hl, r.b); lift_step(ctx, 0x4477, 1);  /* 4476: ld [hl], b */
        liftr_add_hl(&r, r.de); lift_step(ctx, 0x4478, 8);  /* 4477: add hl, de */
        r.c = liftr_dec8(&r, r.c); lift_step(ctx, 0x4479, 4);  /* 4478: dec c */
    } while (lift_branch(ctx, r.c != 0, 0x4476, 12, 0x447B, 8));  /* 4479: jr nz, $4476 */
    lift_step(ctx, 0x447C, 12);  /* 447B: pop de */
    lift_step(ctx, 0x447D, 12);  /* 447C: pop bc */
    lift_tick(ctx, 16); return (DrawTileLine_r){r.hl, r.f_z, r.f_n, r.f_h, r.f_c};  /* 447D: ret */
}

GBLIFT_FUNCTION(0x10, 0x4474, DrawTileLine_binding) {
    LiftRegs in;
    LiftFrame frame;
    lift_own_stack(ctx);
    liftr_from(&in, ctx);
    LIFT_ENTER(frame);
    { const DrawTileLine_r out = DrawTileLine(ctx, in.bc, in.de, in.hl); ctx->hl = out.hl; ... }
    LIFT_LEAVE(frame);
    ctx->a = in.a; ctx->bc = in.bc; ctx->de = in.de;
    lift_ret(ctx);
}
```

**What is written.** Into the folder given with `-o`, or `.lift/convert/`:

- `converted_bank_NN.c`: for each function its C and, under it, the
  *binding*: the lifted function it was, now a few lines that hand the
  machine's registers in and the results back, and leave the rest as the
  line says, kept or undefined;
- `converted.h`: the registers a converted function works in (`LiftRegs`),
  `gblift.h`'s arithmetic written for them (made from that header's own
  text, so there is one place it is right), a result type for each function
  that gives more than one thing, each function's prototype, and the calls
  of it through the machine;
- `convert.json` (`gbrecomp.lift-convert`, version 2): which functions were
  converted, which way each came out and why, what each is passed beyond
  what its line says, why the rest were left, and the checker's verdict.

**Two ways a function comes out.**

*As C.* Its registers are locals and nothing else. A pair it pushes and
takes off again is a local (`s0 = r.bc; ... r.de = s0;`), and where the
pair is only saved to be put back, nothing is kept at all: the caller's
registers are the caller's. A call of another function that C can call is a
C call, `r.a = Add(ctx, r.a, r.b);`, with nothing under it. A jump into such
a function is a call of it and a return. The function does not push what
the original pushes, and its binding says so (`lift_own_stack`).

*On the machine's stack.* Its pushes and pops are the machine's, and so is
the return address under every call. It starts from the machine's registers
for what it does not take and leaves them as its own are, and what a callee
may save is worked out before the call: the stack is then the original's in
every state. A jump out of it hands every register to the machine and
leaves.

In both, a call is written with arguments where `build` has held the callee
to its line. A callee C cannot call directly is called through the machine,
by its address, with the return address the original pushes: `Name_call`.
Its arguments are what the callee takes and its result what it gives, and
it carries every other register across as well, both ways, since what runs
is the machine's code, which may save a register and put it back. A callee
nothing is known of gets every register and gives every one back
(`lift_call_all`).

**What decides.** Five things, none of which a run of the checker can say,
because each is about a state the checker was not in or cannot be in:

- *What it has pushed.* A function is written as C where the number of
  pairs it has pushed is the same by whichever way an instruction is
  reached, never less than none, and none when it leaves; and where
  anything it calls with a pair pushed leaves the stack as it found it.
- *Whether what it goes to comes back.* Assembly can take a return address
  off the stack and return one caller further. C that calls with nothing
  under the call, or turns a jump into a call, changes what such code
  finds there. So the export is asked which functions return to their
  caller with the stack as they found it, whatever they call
  (`well_nested`): balanced themselves, with no jump the export cannot
  follow, and calling and jumping only to such functions. Only those does
  C call directly, and only into those is a jump a call and a return. A
  jump into anything else stays a jump, and a function that makes one is
  called by the machine only. The checker throws away every state in which
  the original does not come back to its caller, so it never sees this.
- *What it may read.* A line of `signatures.txt` says what was seen to
  matter, from the states the checker tried. A register read only on a
  path none of them took is not in it, and a lifted function has the
  register all the same. A C function has what it is passed and nothing
  else. So it is passed whatever its code may read on any path: what the
  line says, and what the code, and the functions it calls, say more. That
  is worked out over all the functions together until none may read more
  than it is passed; a function that is not converted may read what the
  export says it needs.
- *What it gives and what it keeps.* The same holds of what comes out. A
  line says "keeps" of what came out as it went in from every state that
  was tried, and a path none of them took may change it. A caller written
  as C goes on with the value it had, so it has to be sure. A function
  written as C keeps what its code is sure to leave as it found it: never
  written, or pushed as it came and taken off again into the same pair,
  and left so by everything it calls. What its line calls kept without
  the code bearing that out, it gives: the value its own code ended with.
  And it gives whatever a caller among the converted functions may read
  after it, whether or not the line says so. A function that is passed or
  gives more than its line says has the line's words in `signatures.txt`
  and the wider ones in its prototype, and `convert.json` lists both.
- *When an interrupt is taken.* Through the machine, the time of a call or
  a return is only added: the machine takes an interrupt that is due
  before it runs anything else. From C to C nothing would until the next
  instruction ended, and the handler would run one instruction late. So
  there the time is passed with `lift_tick`, which takes it. The checker
  delivers no interrupts; a game that keeps random numbers from the
  hardware's counter shows it within a few frames.

**Left as they are**, each counted with its reason: a function that jumps
through a register, one that returns from an interrupt, one that reckons
with the stack pointer, one somebody changed, and one `build` has not held
to its line and found to match.

**Every one is checked.** Each converted function is given to the checker
held to its line, as `build` gives it a lifted one, with the folder's
`scratch.txt` left out of the comparison. A function that does not hold as
C is written again on the machine's stack, where the conversion changes
less, and everything is checked again, since what called it as C now calls
it through the machine. `convert` exits 1 if one still differs.

A function written as C says `lift_own_stack(ctx)` first thing in its
binding (see `gblift.h`). The checker then compares a state stopped at a
wait without the stack pointer and what lies under where the function was
entered, and throws away a state in which the original read or wrote what
it had pushed as data, where the two differ. Where the two end alike from
such a state and only the time is another, the state stands and its time is
not compared: the original took one branch or the other over a byte it had
pushed, which the C keeps in a local. A function that does not say it is
held to those states as before, the time included.

**Into the folder.** `--write` puts every converted function that matches
where its lifted function stands in `src/`, and `converted.h` and its one
`.c` file into `src/converted/`. They are then functions somebody changed,
as functions written by hand are: `source` keeps them, and `build` checks
them again and builds the game with them. They call each other by name, so
they do not come out one at a time: keep a copy of the folder. A style of
the tool's own that `source` keeps up to date is not written.

`generated_lift_convert_end_to_end` is the test. Its small game has a
function with two arguments and a result, calls from C to C, a pair that is
only saved and one that is moved through the stack, a jump into a function
that comes back and one into a function that sometimes returns past its
caller, a call made with a pair pushed to something that may not come back,
and a jump through a register. It holds each to the ROM, writes them into
the folder, and plays the game built from them against the game built from
the lifted source: the same count of cycles, and the same sum of the timer
as the frame interrupt found it, which a handler that runs an instruction
late gets wrong. It also asks the converter about a function that runs on
into the next one past a conditional return, with lines that leave out what
the next one reads. And it has a function that takes longer when a byte is
not zero, the byte being where the checker's stack has the pair the
function pushes: as C its time is not compared in those states and is the
original's in all the others, and as lifted source it is the original's in
every state.

**On Pokémon Red**, 3,277 of the folder's 6,243 functions are converted and
all 3,277 match held to their lines: 3,004 as C and 273 on the machine's
stack. Of the 3,004, C can call 1,213 directly, and 1,791 are for the
machine to call, nearly all because they leave by a jump into code the
export cannot say comes back. Of the calls, 2,161 are from C to C, 2,602
go through the machine with arguments, and 410 hand every register over.
893 jumps out of a function became a call and a return; 1,866 stayed
jumps. Only 189 functions still keep a pushed pair in a local: the rest of
the pushes were saves, and are gone. 1,730 functions are passed more than
their line says they take, and 762 give more.

The rest: 1,798 the checker cannot run as lifted functions either, 756
have no line, 291 reckon with the stack pointer, 68 jump through a
register, 44 return from an interrupt, and 9 are the ones written by hand.

Written into a copy of the folder, the game built with them was played
against the original from a fresh boot and from the 204 save states along
the story, with buttons at random, 3,000 frames a run: 626 of 626 runs end
with the same count of cycles and the same work RAM, high RAM and cartridge
RAM, the stack and `scratch.txt` aside.

That took four tries, and each failure is one of the things above. The
checker passed every function each time; the game did not:

| What was missing | Runs that ended the same |
|---|---|
| Everything: saved registers as locals, C calls, jumps as calls, on the checker's word alone | 1 of 27 |
| With the interrupt taken where the original takes it | 3 of 27 |
| With functions passed what they may read, and jumps and C calls only into what comes back | 27 of 27, and 620 of 626 |
| With what a function gives and keeps taken from its code, and a conditional return that runs on into the next function read as both | 626 of 626 |

Until functions were passed what they may read and jumps were held to what
comes back, 526 of the functions written as C did not hold when checked and
fell back to the machine's stack; since then, 12. The last fault was found
by building the game with half of the functions kept on the machine's
stack, and half of those, until one was left.

**Not done.**

- **A style of the tool's own.** What `--write` puts into the folder is
  the folder's from then on.
- **Jumps that stay jumps.** Most functions that leave by a jump go into
  code the export cannot say comes back, and are called by the machine
  only. Knowing more of where helpers go (a far call's target) is what
  would turn those into calls.
- **Far calls by name.** A call through a bank-switching helper is a call
  of the helper, with the bank and the address in registers.
- **Names, and bodies.** The registers are `r.a` and `r.hl`, and the flag
  arithmetic is the lifted function's.

## A first run

```bash
# 1. Describe the ROM.
./build/bin/gbrecomp game.gb --pointer-scan strict --lift-export output/lift/game.json
python3 tools/gb_lift.py summary output/lift/game.json

# 1b. Find the calls the game makes through its bank-switching helpers, and
#     describe it again with them. Repeat the pair until `edges` says the
#     export has every one (two or three rounds). See "Calls through helpers".
python3 tools/gb_lift.py edges output/lift/game.json --rom game.gb -o output/lift/edges.json
./build/bin/gbrecomp game.gb --pointer-scan strict --lift-edges output/lift/edges.json \
    --lift-export output/lift/game.json

# 1c. For the tables of functions no helper is seen to call through: say where
#     each is and what shape it has, and give `edges` that (then 1b again).
#     See "Tables somebody has read".
python3 tools/gb_lift.py edges output/lift/game.json --rom game.gb \
    --tables output/lift/tables.json -o output/lift/edges.json

# 1d. For what the ROM cannot say: build the game, play it with --evidence,
#     and give `edges` what it recorded (then 1b again). See "Evidence from playing".
#     Or have the game play itself: see "The game exploring itself".
./output/game/build/game --evidence output/lift/evidence.json
python3 tools/gb_lift.py explore output/game/build/game -o output/lift/explore --minutes 60
python3 tools/gb_lift.py edges output/lift/game.json --rom game.gb \
    --evidence output/lift/evidence.json --evidence output/lift/explore/evidence.json \
    -o output/lift/edges.json

# 2. Read one function.
python3 tools/gb_lift.py function output/lift/game.json 00:2881

# 3. Write a first C version of it (or of everything: --all). Add
#    --style structured for the version meant to be read; see "What the tool writes".
python3 tools/gb_lift.py baseline output/lift/game.json --function 00:2881 -o output/lift/src

# 4. Check C against the ROM. Edit the C, run this again. Add --image FILE,
#    once or more, to check from the game's own memory too, and --entries FILE
#    to check from the states a played game entered each function in; see
#    "The states".
python3 tools/gb_lift.py check output/lift/game.json --rom game.gb --source output/lift/src
```

`check` needs a C compiler on `PATH` (`cc`, `gcc`, `clang`, or `cl` inside a
Visual Studio environment), or `--cc`. It exits 0 when nothing mismatched, 1 on
a mismatch, 3 when nothing could be compared.

```bash
# 5. Gather the functions that matched into a package, and build the game with them.
python3 tools/gb_lift.py check output/lift/game.json --rom game.gb --source output/lift/src \
    --json output/lift/report.json
python3 tools/gb_lift.py package output/lift/game.json --source output/lift/src \
    --report output/lift/report.json -o output/lift/package
./build/bin/gbrecomp game.gb --pointer-scan strict --lift-edges output/lift/edges.json \
    --lift output/lift/package/lift.json -o output/game
cmake -G Ninja -S output/game -B output/game/build && ninja -C output/game/build

# 6. Mod it: edit output/game/lift/*.c, run ninja again, play.
./output/game/build/game              # with the lifted functions
./output/game/build/game --no-lift    # the same executable, original code only

# 7. See what a session ran as lifted C and what it did not. See "What ran".
./output/game/build/game --trace-entries logs/game.trace
python3 tools/gb_lift.py coverage output/lift/game.json --trace logs/game.trace \
    --package output/lift/package/lift.json --report output/lift/report.json
```

Give `--lift` the same analysis options the export was made with,
`--lift-edges` included: a lifted function is hooked where its first block
starts, and the block has to exist.

Step 3 writes a mechanical version: one group of statements per instruction,
`goto` between blocks, the guest's time counted. It is not readable. It is the
version known to be right, and the thing to rewrite:

```c
/* before: tools/gb_lift.py baseline */
GBLIFT_FUNCTION(0x00, 0x2881, lifted_00_2881) {
    lift_advance(ctx, 11);  /* 2881: ldh a, [$FFFF] */
    ctx->a = lift_read8(ctx, 0xFFFF);
    ctx->pc = 0x2883;
    lift_tick(ctx, 1);
    lift_advance(ctx, 11);  /* 2883: ldh [$FFD2], a */
    lift_write8(ctx, 0xFFD2, ctx->a);
    ...
}

/* after: by hand or by a model; `check` still reports a match */
GBLIFT_FUNCTION(0x00, 0x2881, lcd_off) {
    const uint8_t saved_ie = lift_read8(ctx, 0xFFFF);
    lift_write8(ctx, 0xFFD2, saved_ie);
    while (lift_read8(ctx, 0xFF44) != 0x91) {
    }
    const uint8_t lcdc = lift_read8(ctx, 0xFF40) & 0x7F;
    lift_write8(ctx, 0xFF40, lcdc);
    ...
}
```

## The export

`gbrecomp ROM --lift-export FILE` analyses the ROM, writes the file and
generates no code. It takes the analysis options (`--pointer-scan`,
`--no-scan`, `--symbols`, `--annotations`, ...); names from a `.sym` file show
up throughout. Numbers are decimal, as JSON wants; `gb_lift.py function` prints
them in hexadecimal.

Top level: `schema` (`gbrecomp.lift-export`), `version` (1), `rom` (title,
SHA-256, size, mapper, bank count), `analysis_profile`, `stats`, and three
lists.

### `functions`

| Field | Meaning |
|---|---|
| `id` | `gbfn:v1:BBBB:AAAA`, bank and address in hexadecimal. The same ID `NATIVE_PATCHES.md` uses. |
| `name`, `symbol` | The generated name, and the `.sym` name if one was loaded. |
| `kind` | How the function was found; see below. |
| `recompiler_function` | The recompiler emits a function at this address too, so a native patch can bind it. |
| `hazards` | What makes the function awkward to lift; see below. |
| `registers`, `flags` | `reads`: may be read before being written, so an input. `needs`: the same, with code the export cannot follow counted as reading everything. `writes`: may be written. `always_writes`: written on every way out, a call that may not come back included. `read_after`: what may be read once the function has returned. Callees are included. See [What a function owes](#what-a-function-owes). |
| `summary_incomplete` | An unknown callee or exit is involved, or a return that does not go to the caller: `reads` and `writes` may be too small. `needs` and `always_writes` hold all the same. |
| `stack_balanced` | Every `RET` pops the address the call pushed. False for a function that pops its return address, pushes an address and returns to it, moves SP, or is reached with the stack at two different depths. |
| `entered_by` | Ways in that no call or jump in the decoded code accounts for. Empty: none was found. |
| `pushes`, `pops` | Register pairs the function pushes and pops. A pair in both is usually saved, not used. |
| `calls`, `jumps_out` | Calls, and jumps or fall-through that leave the function, each with the target function when it is known. `known`: the constants in the registers there, followed through the function. `reaches`: where the helper it calls goes on to from here; `reaches_table`: the table it picks from, with its entries; `reaches_seen`: where a run of the game went on to from here. All three once `--lift-edges` has said (see [Calls through helpers](#calls-through-helpers) and [Evidence from playing](#evidence-from-playing)). |
| `seen_jumps` | For a computed jump in the function itself (a `JP HL`, a `RET` used as one): where a run of the game saw it go. |
| `ran` | Present when the edges came with evidence: whether a run of the game arrived at the function's first instruction by a call, an interrupt or such a jump. |
| `declared_table` | Present when a declared table names the function (see [Tables somebody has read](#tables-somebody-has-read)): the table's `name`, its `bank` and `address`, and the `index` of the entry. The first such table, if several do. |
| `callers` | Functions that call or jump here. |
| `memory` | Fixed addresses the function reads (`r`), writes (`w`), or loads into a register pair (`p`), with hardware register names. |
| `bank_writes` | Writes to the cartridge mapper, with the value when it is a constant. |
| `rom_references` | 16-bit constants that are ROM addresses, and whether they point at code or data. |
| `blocks` | The basic blocks with their instructions: address, bytes, RGBDS-style text, cycles, jump target. On a block that ends in a `JP HL` the function expects back from, `jump_returns_to` lists the blocks of the function its callee returns to, and `jump_returns_certain` says whether the code itself pushes that address (see [Calls made by hand](#calls-made-by-hand)). |

The recompiler's own functions are units of code generation: the point a call
returns to starts a new one. The export does not use them. A function here
starts where something calls, and holds the blocks that jumps and fall-through
reach from there without entering another function. A block two functions
would share becomes a function of its own, so every block has one owner.

| `kind` | Found because |
|---|---|
| `entry`, `interrupt` | The cartridge entry point, an interrupt vector. |
| `call` | A `CALL` or `RST` targets it. The most trustworthy kind. |
| `far_call` | A call through a helper reaches it (`--lift-edges`). As trustworthy: the ROM's own code names the bank and the address. |
| `table_call` | It is an entry of a table of functions a helper calls through (`--lift-edges`), in a table that ends where code begins, or is as long as somebody says. |
| `declared_table` | It is an entry of a table of functions somebody declared (`edges --tables`, `--lift-edges`): `declared_table` says which entry of which table. As trustworthy as the declaration. |
| `seen` | A run of the game jumped or called there by a jump its code does not spell out, or a `CALL` arrived there whose bank the code around it does not show (`--evidence`). It is code; what else reaches it, and where else that jump goes, the run does not say. |
| `jump_table` | The analyzer resolved a `JP HL` to it, or it is an address pushed before one that the function pushing it does not go on from (see [Calls made by hand](#calls-made-by-hand)). |
| `declared` | A `.sym` file or `--annotations` says so. |
| `shared_tail` | Two functions jump or run into it. |
| `detached_tail` | A stretch of one function that does not follow on from any of its blocks in the ROM, and that the function reaches only by `JP`: another routine it ends by jumping to. The function is its caller. A rule of thumb, right four times in five on Pokémon Red (see [Scoring against a disassembly](#scoring-against-a-disassembly)); where it is wrong the cut is still at a jump and the code still checks. Not made in `unreferenced` functions. |
| `unreferenced` | The scans decoded it and nothing above reaches it. Either a handler reached through a table or a bank the analyzer could not resolve, or data that happens to decode. |

| Hazard | Meaning |
|---|---|
| `unknown_exit` | Control leaves to code that was not decoded. |
| `unresolved_call`, `calls_ram` | A call whose target function is not known, or is in RAM. |
| `computed_jump` | It has a `JP HL`. |
| `sp_manipulation` | It sets or does arithmetic on SP. |
| `bank_switch` | It writes the cartridge mapper. |
| `halt`, `stop`, `interrupt_control` | It uses `HALT`, `STOP`, or `DI`/`EI`/`RETI`. |
| `reads_volatile_io` | It reads a register the hardware changes on its own (LY, STAT, DIV, the joypad, ...), so it depends on when it runs. |
| `interrupt_handler`, `ram_code`, `lower_window_bank` | An interrupt vector; code that runs from RAM; a bank mapped at `0x0000` (MBC1 mode 1). The last two cannot be lifted this way. |
| `undefined_opcode` | It decodes to a byte that is no instruction: almost certainly data. |

`stop`, `undefined_opcode` and `sp_manipulation` on an `unreferenced` function
usually mean it is data.

#### What a function owes

Two questions decide which registers and flags code has to get right around
a call. The export answers the first for every function and the second for
few.

**What must hold when it is entered.** `needs` and `always_writes`. Before a
call, a value is still wanted if the callee needs it, or if it is read after
the call and the callee does not always overwrite it. Both sets are safe for
any function the export has: where its code calls something unknown, runs
off into bytes that were not decoded, or returns somewhere other than its
caller, `needs` counts that as reading every register and `always_writes`
counts only what was written before it. The structured style uses them at
every call and at every jump into another function.

**What is read after it returns.** `read_after`. That takes knowing every
caller, and assembly does not say: a function is also entered through a
table of addresses, or through a helper that switches bank and jumps to a
register. So `read_after` is every register and flag unless `stack_balanced`
is true and `entered_by` is empty, and `entered_by` lists the evidence of
another way in:

| `entered_by` | Evidence |
|---|---|
| `entry`, `interrupt`, `jump_table`, `declared`, `unreferenced` | Its `kind`: not found by a call. `jump_table` also where a call found it and the analyzer resolved a computed jump to it as well. |
| `address_loaded` | Its address is loaded into a register pair somewhere, to be jumped to or pushed. Not counted where the load is half of a far call the export has followed. |
| `far_call`, `table_call` | It is called through a helper. Those callers are listed, but it returns into the helper, and what is read after that is not worked out. |
| `declared_table` | A declared table names it. Whatever reads that table can call it, and nothing lists that. |
| `seen` | A run of the game reached it by a jump its code does not spell out, or by a call nothing here accounts for. The callers listed are the ones that run met, and for such a call there is none. |
| `data_pointer` | Its address is a word in data in its own bank or the home bank, or anywhere next to its bank number. A weak sign: a third of Pokémon Red's functions have it, most of them by chance. |

With none of that, `read_after` is what its callers read after the call
before overwriting it, and what the callers of a function that jumps into it
read. It is evidence, not proof: an address computed at run time leaves no
trace. It also over-states. A caller that is data decoded as code counts,
and a caller whose own callers are unknown hands "everything" to whatever it
calls last. Nothing drops code on the strength of `read_after` yet; the
structured style prints it above the function. See [Not done](#not-done).

### `data_regions`

Every run of ROM bytes that no decoded instruction covers: bank, address, size,
ROM offset, and a guess from the bytes (`header`, `padding`, `ascii_text`,
`pointer_table`, `unknown`). `classified_by_analyzer` says the recompiler's
data classifier had marked it; `referenced_by` lists functions that load a
pointer into it, which is where to start when working out what a table is.
`tools/gb_data.py split` writes each of them to a file of its own, to be
edited and put back into a ROM image the game loads (`DATA_FILES.md`).

### `memory`

Every fixed RAM or hardware-register address the code names, with the functions
that read it, write it, or take its address. Addresses with many `pointer_loads`
are the bases of tables.

## Calls through helpers

A game with more than one bank of code cannot `CALL` from one bank into
another. It loads the bank into one register and the address into a pair and
calls a helper in the home bank that switches bank, jumps to the pair, and
switches back. Or it loads a number and calls a helper that looks the
function up in a table. An analysis that follows `CALL`s sees a call to the
helper and nothing after it. So the function at the far end has no caller in
the export, is often not a function in it at all (it is part of whatever
decoded code runs into it), and sometimes was never decoded.

Where the helper is called, though, the registers it reads usually hold
constants, and the export records them (`known` on each call). `gb_lift.py
edges` asks the ROM the rest:

```bash
python3 tools/gb_lift.py edges output/lift/game.json --rom game.gb -o output/lift/edges.json
```

For every call or jump, with a constant in some register, to a function the
export cannot follow to its end, it runs that function in the checker's
interpreter (`lift/checker/lift_probe.c`): from the ROM, with those
constants, everything else in the machine made up, six times over. The run
stops at the first transfer whose target the code does not spell out, a
`JP HL` or a `RET` to an address no `CALL` pushed. If that is the same
address with the same bank mapped all six times, the constants decided it,
and that is where this call goes. It is then run six times more with the
constants made up too: a function that still goes to the same place makes a
far call of its own, which is found where it makes it, and the call to it
is an ordinary call. Nothing about the game is assumed: not which function
the helper is, not which registers it reads.

**Tables.** A helper that looks its target up in a table goes somewhere
different each time, because the number it is given is not a constant where
it is called. But the run shows where each address came from: two bytes of
the ROM, some entries on from a constant the call did load. Which constant
is found by moving each a little and running the same states again: the one
the entries move with is the table. Its entries are then read straight from
the ROM. How far the table goes is the one thing the ROM does not say, and
reading past its end makes "functions" of whatever follows (in Pokémon Red,
usually a table of pointers to text). So a table is taken only when it ends
exactly where code is known to begin: at the lowest address one of its own
entries points to in its bank, or at a function something calls. A table
that runs into a byte that is no address first, or ends a byte short, is
counted and left alone.

The file it writes (`gbrecomp.lift-edges`, version 1) lists each call: the
function and address it is made from, the helper, the bank and address it
reaches, where the helper has the target return to, and for an entry of a
table, the table and the entry's number. Hand it back with `--lift-edges`,
to the export and to the build alike:

- every target is an entry point for the analysis, so its code is decoded
  and compiled whether or not anything else reaches it. Such an entry is
  code on the ROM's own word: the pass that resolves overlapping decodes
  does not drop it for a misaligned decode that covers more bytes, as it
  would an `--add-entry-point`;
- in the export each target is a function of its own, `kind` `far_call` or
  `table_call`, with the functions that reach it in `callers`, and on the
  call `reaches`, or `reaches_table` with the table's entries in order;
- the structured style names it at the call:
  `lift_call(ctx, 0x35D6 /* Bankswitch: on to Random_ */, 0x3E67)`,
  `lift_jump(ctx, 0x3D97 /* CallFunctionInTable: on to one of the 7 in the
  table at 0x4E73 */)`.

Newly decoded code has far calls of its own, so run the pair again until
`edges` reports that the export has every call it found.

What this does not find: a table whose end nothing marks; a table of
anything but two-byte addresses, or one the helper itself names; an address
kept in RAM (a map's script, copied out of its header when the map loads);
and a helper that takes its target from bytes after the `CALL`. `edges`
counts each. A helper also has to get to its jump within `--max-steps`
instructions from a made-up state without waiting for the hardware. The
first three are what the next section is for.

## Tables somebody has read

A game keeps most of its functions in tables, and most of its tables are
none of the above. The script of each room is in the room's header, which
is copied to RAM when the room is entered and jumped through from there.
What an item does is in a table the item's number is doubled into; what a
battle animation does, in a list of three-byte entries that ends with a
byte. Nothing in the code can be followed through them, and playing finds
an entry only by going there.

To whoever has read the code that uses a table, though, where it is and
what shape it has is a line or two. `edges --tables FILE` takes those lines
and reads the tables out of the ROM:

```json
{"schema": "gbrecomp.lift-tables", "version": 1, "tables": [
  {"name": "what each item does", "bank": 3, "address": 21985, "entries": 83,
   "why": "UseItem_ doubles the item's number into it and jumps"},
  {"name": "the script of each room", "bank": 0, "address": 430, "entries": 248,
   "target_bank": {"table": {"bank": 3, "address": 16957}},
   "leads_to": {"entries": 1, "entry_size": 10, "pointer_at": 7},
   "unused": [11, 105],
   "why": "LoadMapHeader copies the header to RAM; the script is jumped to through bytes 7 and 8 of it"}
]}
```

| Field | Meaning |
|---|---|
| `bank`, `address` | Where the table starts. |
| `name`, `why` | What to call it, and how the reader knows. `name` is shown wherever one of its functions is; `why` is for the next reader. |
| `entries` or `ends_with_byte` | How long it is: so many entries, or until an entry whose first byte is this. One of the two. |
| `entry_size`, `pointer_at` | An entry is so many bytes (2), with the address so many bytes into it (0). |
| `target_bank` | The bank of what an address points to: `"same"` as the table (the default), a number, `{"at": n}` for a byte of the entry, or `{"table": {"bank": .., "address": ..}}` for a second table with one byte an entry. An address below 0x4000 is in the home bank whatever this says. |
| `skip` | Addresses that mean no entry (`[0]`). |
| `unused` | Numbers of entries the game never uses, whatever is in them. |
| `leads_to` | The address is not of a function but of another table, of the shape given here (the fields above, without a place or a name): one record with `"entries": 1`, or a list. |

`edges` reads every entry and refuses the file, naming the entry, if one is
not an address in the ROM, is in a bank the ROM does not have, points into
its own table, or points at a byte that is no instruction within a few
instructions. That catches a table read from the wrong place by a wide
margin. It does not catch one read an entry too long into more addresses of
code, and most data decodes as something: a declaration is somebody's word,
as an `--annotations` file is. So for each table it also says how many of
the places it names are functions of the export already. A real table names
a good many that something else found too; one that names hardly any is
asked about.

What happens to the functions:

- a plain table of addresses that a helper is seen to call through, and
  whose end the ROM does not mark, is taken at the length declared and
  stays on the call: its functions are `table_call` with `reaches_table`,
  as if the ROM had marked the end. `edges --unread FILE` writes every such
  table out as a declaration to finish, with a call that goes through it:
  `entries` is for whoever reads that call to fill in;
- every other table goes into the edges file (`declared_tables`: its name,
  where it is, and each entry's number, bank and address). With
  `--lift-edges` each entry is an entry point for the analysis, kept
  whatever the overlap resolution counts, and in the export a function of
  `kind` `declared_table` that says which entry of which table it is:
  `"declared_table": {"name": "the script of each room", "bank": 0,
  "address": 430, "index": 37}`. Nothing says who calls it, so `entered_by`
  has `declared_table` and the checker compares everything it leaves behind.

No tables file for any game is in the repository: it is a reader's notes on
one ROM. For a game with a disassembly the places can be looked up; for one
without, they are what reading the lifted C of the routine that loads a
room, or uses an item, shows.

**On Pokémon Red**, eight declarations (the room headers, what is hidden in
each room, items, move effects, trainer classes, battle animations, the
weighing of a trainer's moves, one walk) name 386 places, and every one is
a label of code in the disassembly. With them and nothing played, 2,041 of
its 2,318 functions are functions of the export (88%), from 1,711 (74%).
Evidence from 4,200 hours of the game played by machine, without the
tables, gets to 1,944. With both, and 41 more declarations, one for each
list of a room's scripts that `--unread` wrote out, with the lengths filled
in: 2,088 (90%), 218 still inside another function and 12 not decoded.
Of the 376 functions that are `declared_table`, the checker matches 372
and mismatches none; of all 6,421 it matches 4,374 where it matched 3,808
before either, and the game built with them ends 3,600 frames in the same
state as with `--no-lift`. (`unused` is cut short in the example above: 22
room numbers have no room.)

Most of the 230 left are not waiting to be found. In the disassembly's own
source 186 of them are only ever jumped to, never called
(`AIUseFullRestore`, which the routines for trainers end by jumping to): their code is
decoded and lifted, inside the function that jumps there, and whether such
a piece is a function of its own is a choice about how to cut the code, not
a fact the ROM withholds. The export makes that choice where the code
shows it (`detached_tail`), which takes the 230 to 159.

## Calls made by hand

The SM83 has no `CALL` through a register, so assembly makes one: it pushes
the address to come back to and jumps.

```
    ld   bc, .Return
    push bc
    jp   hl
.Return
    pop  bc
```

Pokémon Red's helpers of the last section all end this way, and so does
code that picks a handler out of a table. Taken for the jump it looks like, it cuts
the function in two. What follows `.Return` becomes a function that nothing
calls, and in the game the lifted C of the first half is over at the jump:
when the callee returns, the game is in the middle of the function and runs
its recompiled code from there to the end.

The export takes it for a call wherever it can say where the callee comes
back:

- **From the code.** The block that ends in the `JP HL` pushes a register
  pair, with no call and no write of SP after the push, and the pair holds a
  constant. The block at that address then belongs to the function, the
  `JP HL`'s block lists it (`jump_returns_to`, with `jump_returns_certain`
  true), and the entry point the analysis makes of such an address is not a
  function of its own.
- **From a run, or a probe.** The block makes the push, and what it pushes
  is not a constant: the naming screen of Pokémon Red reads both the handler
  and the place to come back to from a table. Then the address is whatever
  `edges` wrote down as `returns_to` for a jump made by that instruction,
  seen in a recorded run (see [Evidence from playing](#evidence-from-playing))
  or found by the probe of a helper. Nothing jumps to such an address, so it
  may be in the middle of a block: the block is cut there.
  `jump_returns_certain` is false, because another run may push another
  address.

Either way the address is not a function because a run arrived there (the
edges file's `entered`, or a `seen` edge that left by a `RET`): the callee
arrives there every time it returns. A `JP HL` seen to go there, a `CALL` to
it or a table that names it makes a function of it as anywhere else, and the
function that jumps then ends at its jump.

Both generators then write the call, with `lift_call_pushed` (see
[Lifted source](#lifted-source)), and go on in the same C function:

```c
    lift_push16(ctx, ctx->bc);  /* 35E2: push bc */
    lift_advance(ctx, 92); if (lift_call_pushed(ctx, ctx->hl, 0x35E4)) return;  /* 35E3: jp hl */
    ctx->bc = lift_pop16(ctx);  /* 35E4: pop bc */
```

Where the address is only one the jump was seen to come back to, the C looks
at what is on the stack first, and any other address leaves the function as
a `JP HL` always did:

```c
    switch (lift_read16(ctx, ctx->sp)) {
    case 0x65F3: if (lift_call_pushed(ctx, ctx->hl, 0x65F3)) return; goto L_65F3;
    case 0x65FC: if (lift_call_pushed(ctx, ctx->hl, 0x65FC)) return; goto L_65FC;
    default: lift_jump(ctx, ctx->hl); return;
    }
```

Nothing is assumed about the word on the stack that the comparison does not
check. Code also pushes things that are no return address before a `JP HL`
(Pokémon Red's text engine pushes the pointer to the text for the handler to
pop), and a run records that word as `returns_to` like any other; it is not
the address of a block of the function, so it is never taken.

What is not taken: an address that is the start of another function, because
something calls it or two functions push it (the jump leaves, and its callee
returns into that function's start, which runs as C all the same); an
address the analysis has not decoded; and a jump whose block does not make
the push itself, since what is on top of the stack there may be the
caller's.

## Evidence from playing

Some jumps the ROM cannot be asked about. A map's script is an address in the
map's header, copied into RAM when the map loads and jumped to from there. A
table of item effects holds three-byte far pointers, and nothing marks where
it ends. For those, play the game:

```bash
./output/game/build/game --evidence output/lift/evidence.json
python3 tools/gb_lift.py edges output/lift/game.json --rom game.gb \
    --evidence output/lift/evidence.json -o output/lift/edges.json
```

With `--evidence FILE` a generated game records every jump it makes that its
code does not spell out: a `JP HL`, and a `RET` that pops an address no `CALL`
pushed (`push hl / ret`). For each it keeps the instruction, where it went
and in which bank, what was on the stack for the target to return to, and
the `CALL` that entered the function it happened in, so that a far call is
put on the call that made it and not on the helper they all go through. It
also keeps every address a call, an interrupt or such a jump arrived at. The
file (`gbrt.evidence`, version 1) is written when the game shuts down.

The recorder (`runtime/src/gbrt_evidence.c`) keeps a shadow of the call
stack, fed from the runtime's own `CALL`, `RST`, `RET` and jump helpers,
which generated and interpreted code alike go through. So nothing is added
to the generated code, an unpaced run loses about a hundredth of its speed,
and the game ends in the same state as without the option. It records the
same headless and in a window: play, close the window, and the file is
there. A game that is killed writes nothing. A session may load a save
state: the calls that were in progress when the state was made return
without being taken for jumps, since the record never saw them made.

A function that was in progress and makes a
[call by hand](#calls-made-by-hand) after the state is loaded has no call
on the shadow stack to measure its push against. The recorder measures
against the stack the state had instead. It knows how far up that stack the
calls from before the state have returned (to begin with, where the stack
pointer was when the state was loaded): a `JP HL` made with the stack
pointer below that was made with something pushed since. That alone does
not make it a call. The code at the bottom of the stack, which is in no
call at all, looks the same when it pushes a register for its target to
pop, and nothing tells the recorder that the calls from before the state
are all back. So such a jump is not written down at once.
If its target returns to the address that was pushed, from the place on the
stack where it was pushed, that is the call made by hand: it is written
down as one, with what it comes back to and with no `CALL` that entered the
function (`"from": null`), and the return is a return like any other. If
the target pops what was pushed, leaves some other way, or has not returned
when the game stops, it is written down as the jump with nothing to come
back to that it would have been.

What that cannot tell:

- a state made between the push and the `JP HL` has the address on its
  stack like anything else it had, and so has one made while the function
  held more on the stack than it does when it jumps. The `JP HL` is written
  down as a jump with nothing to come back to, and the target's `RET` goes
  for one of the returns from before the state;
- a stack that is not in RAM is not read for it, and the jump is written
  down as before;
- `push` twice and a `RET` used the same way, from a function that was in
  progress, is still a `RET` to an address no `CALL` pushed, with nothing
  for its target to come back to.

A recording made before the recorder did this has, for every such call,
the `JP HL` as a jump with nothing to come back to and the callee's `RET`
as one to an address no `CALL` pushed, which is then among the addresses
arrived at. A search that started thousands of runs from save states
(`gb_lift.py explore`) has a few of those for every such helper, and so
has every edges file made from one; `edges` and the export know them by
the address.

Lifted source does not go through those helpers: a game built with `--lift`
runs its recompiled bodies while it records.

`edges --evidence FILE` (repeatable, for several sessions) merges what was
seen with what it finds in the ROM:

- a jump seen from a call the ROM already answered for is checked against
  that answer; one that went elsewhere is counted and kept, and is worth a
  look, since it means the probe's answer did not hold;
- any other becomes an edge marked `seen`: on the `CALL` that entered the
  function, if the export has that call (`reaches_seen`), otherwise on the
  jump itself (`seen_jumps`);
- what was on the stack for the target to return to, where the jump was
  made with something pushed, is where that jump comes back: the function
  goes on from there in its lifted C
  (see [Calls made by hand](#calls-made-by-hand));
- a `RET` seen to go where a function's own `JP HL` comes back
  (`jump_returns_to` in the export) is that call returning, recorded from a
  save state made inside the function where the recorder could not tell the
  call (see above): it is counted and left out;
- the addresses arrived at become `ran` on each function, and one that is
  not the start of a function is made one, unless it is such an address,
  which stays the function's own. Where a `CALL` or an interrupt
  arrived, a function starts, whatever the map made of it; the usual case is
  a call into a bank read out of a variable, whose target the map had merged
  into whatever happened to decode before it.

In the next export the targets are functions of the `kind` `seen`, and the
generated game compiles them. The structured style says
`lift_call(ctx, 0x101B /* RunMapScript: seen going on to RedsHouse2F_Script */, ...)`.

Evidence is true of the runs it came from and of nothing else. A call seen
going to three places may go to a fourth; a function that did not run is
not thereby data. It finds what was played, so what it adds grows with how
much of the game the sessions cover.

## The game exploring itself

Nobody has the hours to play every corner of every game, and buttons pressed
blind do not get far: 40,000 frames of them leave Pokémon Red in the room it
starts in. `gb_lift.py explore` plays instead, many copies of the game at
once, knowing nothing about it:

```bash
python3 tools/gb_lift.py explore output/game/build/game -o output/lift/explore \
    --minutes 60 --export output/lift/game.json
```

It is a search by selection. Each run starts from a moment an earlier run
reached, which is a save state, and presses buttons at random for 4 to 16
seconds of the game. The run's last moment is kept, and can be a parent in
turn, when the run found something no run had:

- **code**: an address a call or a jump arrived at for the first time, or a
  jump going somewhere new. This is what the search is for;
- **a screen** no run has ended on: the background tiles in view, read from
  the save state's video memory. Tile numbers and not pixels, so that an
  animated tile or a moving sprite does not make a screen new. Crossing a
  room runs no new code, and every step of the way shows something else, so
  this is what gets a run to the door;
- **a value** in a byte of the game's memory that has held few (64 at most;
  a byte that has held more is a counter or a random number and is left
  out from then on). The number of the room is such a byte, and so is a
  flag that a conversation sets.

A parent is picked from a dozen of the moments kept, half of them from the
latest fifth: the later a moment was found, the further along it is likely
to be. Among them the ones that found code count for most, and every try
already made counts against a moment. A screen that differs only in its
text is as new as one from another room, and a game shows a great many of
those, so tries are also counted by what a screen looks like from a
distance (the commonest tile in each twelfth of it), and only the latest
four hundred moments of one such view are kept (two dozen was tried: a
cave looks the same from a distance everywhere, and the search kept
forgetting the far end of it). A moment that found no code
and has led nowhere in eight tries is dropped, and past `--keep` moments
(20,000 if not given, a quarter of a megabyte each) the ones least far
along go. What they found stays in the record.

**Checkpoints.** Left to itself the search finds what is near what it has
reached, and a game's story is a long errand done in the right order. A
walkthrough can be handed to it as a list of checkpoints, each a name and
conditions on bytes of the game's memory, in the order a player meets them:

```json
{"schema": "gbrecomp.lift-goals", "version": 1, "goals": [
  {"name": "Has the parcel",       "when": [{"address": "D74E", "bit": 1}]},
  {"name": "Parcel: back in town", "when": [{"address": "D74E", "bit": 1}, {"address": "D35E", "equals": 0}]},
  {"name": "First badge",          "when": [{"address": "D356", "bit": 0}]}
]}
```

An address is in work RAM or high RAM; a condition is `equals`, `not`,
`at_least`, `at_most` or `bit`. A moment is as far along as the last
checkpoint whose conditions all hold in it, so one that walked back, or
lost a fight and woke up two towns ago, counts as where it is. With
`--goals FILE` three candidates in four for a parent come from the moments
of one checkpoint (the furthest two times in three, the one before it two
times in three of the rest, and so on back), every screen counts as new
again past each checkpoint (the
way back through a room, with what was fetched, is ground not yet
covered), and the first moment past each checkpoint is copied to
`checkpoints/NNN-name.state`: a state to start from another day, to play
from yourself, or to make an image of for the checker
(see [The states](#the-states)). The list is the one thing here that knows
the game. Which byte holds the room and which bit a badge is what a
disassembly's symbol file says, or what `tools/gb_discover.py` finds.

A list has to name every room on the way. A moment in a room it leaves out
counts as back where the last flag was set, so the search turns away from
the very door it has to go through. When more than a third of the runs
from the furthest moments end at one earlier checkpoint, the progress
lines say so.

What else a list wants, each learnt on Pokémon Red by a search that stood
still:

- **A waypoint every few squares.** Between two checkpoints the search has
  only what is new to go by, and a menu has more new screens than a road.
  With boxes on the bytes that hold the player's position, a few squares
  apart along the way, a leg that took an hour took a minute. Read them
  off the game's map data, not off pictures: a cave floor in two heights
  looks like one room in a picture.
- **A waypoint the way goes through.** A box that is a blind corner becomes
  the whole of the furthest moments, and every step out of it counts as
  falling behind. The search always spends a third of its runs further
  back, so such a box costs time and not the search; it is still a wrong
  box.
- **A fight that has to be won, as steps of its own:** being in it, then
  each further Pokémon the other side sends out. The moments of one fight
  are a handful among thousands of menu screens at the same checkpoint, and
  get a handful of the runs: one fight took twelve minutes so, and seconds
  with its steps in the list.
- **A flag nothing can set early.** A checkpoint that is only a flag
  (a guard given his drink) puts every moment with the flag at its place in
  the list, however far behind it stands. If the flag can be set before the
  search gets there, tie it to the room.
- **A position can lie for a moment.** Half way through a door the room's
  number is already the new one and the position still the old room's. A
  box in the new room that holds the old room's door will be passed by a
  state saved right then.

Every run is the generated game as it is, started with `--load-state-file`,
`--input`, `--evidence`, `--save-state-file` and `--dump-state`, with a
`--save-dir` of its own, at a priority below the desk's other work. What
comes out, in the folder given:

- `evidence.json`: everything the runs recorded, merged, in the file format
  a played session writes. Give it to `edges --evidence`, with a session of
  your own as well if you have one;
- `states/`: the moments kept, and `explore.json`, which says how each was
  come by. Run the command again with the same folder and it goes on from
  there, so an hour now and an hour tonight are two hours;
- `checkpoints/`: with `--goals`, the first moment past each checkpoint.

`--state FILE` (repeatable) starts from a save state somebody made as well
as from power-on, or with `--states-only` from those alone. That is how to
get it past what it cannot do alone: play to the next town, save a state
from the menu, hand it over. A state can also be one somebody edited
(`tools/gb_discover.py` writes bytes into one): a party strong enough that
a fight is not what stops the search finds the same code on the far side
of the fight, as long as every value written is one the game could hold,
and less of the code of the fight itself. `--hold ADDRESS=VALUE`
(repeatable, hexadecimal) writes a byte of the game's memory at the start
of every run: the steps a repellent has left, so that a cave is a cave to
cross and not two thousand fights. `--export`
makes the progress lines say how many of the places arrived at are not yet
the start of a function, which is the number that matters. `--workers`
sets how many copies run at once (all processors but two if not given).

What it is not: a player. It does not know what winning is. Without
checkpoints it finds what is near what it has reached; with them it still
has to stumble on each step between two of them, a fight it can lose
included, and a step that takes a particular menu in a particular place
may take it a long time. The furthest moments are not always the right
ones to go on from (the wrong staircase of three, a checkpoint whose
conditions hold in a blind corner), which is why a third of the runs
always start from further back.

**On Pokémon Red**, 18 copies on a 20-processor desk run about 70 runs and
38,000 frames a second, 600 times the game's own speed:

| What a moment is kept for | How far, and when |
|---|---|
| Buttons pressed blind, one run of 40,000 frames | Never leaves the first room. 242 of the disassembly's 2,318 functions arrived at |
| New code only | 670 functions in 10 minutes; stops in the room where the first fight is |
| And a screen not seen | The same 670 in one minute; out onto the first route in three and a half, and no further in fifteen |
| And a list of 55 checkpoints, to the third badge | The parcel fetched from the next town in two and a half minutes |
| And a party edited to level 100, a repellent held on | The first badge 13 minutes on. The cave after it took an hour and a half and three changes to the search; the second badge three minutes after the cave; the third after two more waypoints |
| And a list written as above (321 checkpoints to the eighth badge), what errands fetch held in the bag | Third badge to fourth, through Rock Tunnel and the tower in Lavender: an hour, most of it three faulty checkpoints being found. Between faults a leg took about a minute: Celadon's gym to the top of Silph Co., a locked door and two fights on the way, in two |

What the runs recorded up to the third badge, 3,400 hours of the game
with nothing dropped, given to `edges --evidence` beside the 40,000 blind
frames: 1,842 different jumps the code does not spell out where the blind
run had 60, and 1,275 functions entered where it had 255. Of the
disassembly's functions, 1,852 are functions of the export made with them
(80%), from 1,711 (74%); 389 are still inside another function and 77 not
decoded, from 488 and 119. The checker matches 4,061 of 6,105 and
mismatches none, 249 more than before, and the game built with them ends
3,600 frames in the same state as with `--no-lift`. (At the second badge,
765 hours in, the same figures were 1,278, 1,081, 1,808 and 3,970.)

Three things went wrong on the way and are worth knowing. A list that
leaves out a room stops the search at its door (above). Buttons are timed
by a count of cycles, and of the game's two the one a state dump calls
`cycles` starts again every seventeen minutes: timed by it, every line of
moments longer than that went deaf, which looks exactly like a search that
is stuck; `total_cycles` is the one. And loading a state while recording
made the recorder take returns for jumps
(see [Evidence from playing](#evidence-from-playing)).

## What the game ran

Nothing in a ROM says which bytes are instructions. The analysis decodes
what it can reach and what looks like code, and part of what it decodes is
a picture, a map or a table whose bytes happen to read as instructions:
functions that are no functions. They cannot be told by running them,
since nearly any bytes are instructions and do something. What tells them
is what the game does with those bytes when it is played.

```bash
./output/game/build/game --executed logs/played.executed --data-use logs/played.use.json
python3 tools/gb_lift.py executed output/lift/game.json --map logs/played.executed \
    --data-use logs/played.use.json --verdicts output/lift/report.json --json logs/sorted.json
```

`--executed FILE` has the game run in the interpreter and note every byte
of the ROM that was part of an instruction it executed, the first byte of
an instruction apart from the later ones. `--data-use` (`DATA_FILES.md`)
notes every byte it read as data. Both can be kept in one run, and the game
ends in the same state as without. `gb_lift.py executed` puts any number of
each together (`--map` and `--data-use` are repeatable, and what any file
has counts) and sorts the export's functions:

| Class | What the game did | What it says |
|---|---|---|
| `ran` | Executed its first instruction. | It is code, and it begins there. |
| `ran_inside` | Executed instructions of it, never the first. | It is code, entered further in: what is before that is not where it begins. |
| `ran_otherwise` | Executed bytes of it, but as other instructions than the export decoded. | The export read real code at the wrong byte. |
| `read` | Executed none of it and read bytes of it as data. | It is data, with one exception: code the game copies into RAM and runs there is read and never executed where it lies. |
| `neither` | Neither. | Nothing: code the game did not get to in those runs, or data it did not read. |

The command counts the classes by how the export came by each function and,
with `--verdicts` (a `check --json` report or a source folder's
`lift-source.json`), by the checker's verdict. It also counts by the byte,
and says how many executed bytes are in no instruction of the export, which
is code the export does not have. `--json` has every function's class with
how many of its bytes were executed and how many read.

Both records are of what was played. `ran` is proof; `read` and `neither`
are as good as the playing was wide.

On Pokémon Red, over the three recorded plays of 1,365,841 frames (see
[The states](#the-states)), the game executed 4.67 thousand million
instructions, 80,636 different bytes of the ROM. No executed byte lies
outside the export's instructions. Of the 173,318 bytes the export has in
instructions, 29,031 were read as data and never executed, and 63,651
neither. Of its 6,396 functions:

| | `ran` | `ran_inside` | `ran_otherwise` | `read` | `neither` |
|---|---|---|---|---|---|
| all | 2,107 | 5 | 7 | 2,101 | 2,176 |
| the 4,681 that match | 2,057 | 4 | 4 | 1,006 | 1,610 |
| the 1,713 with no verdict | 50 | 1 | 3 | 1,094 | 565 |

Held against the disassembly, the 2,101 that were read and never executed
are data: 1,699 lie in something it has as data, 351 under a label whose
kind could not be told, and 51 in or at a function. Of those 51, 48 are a
table, a string or a list of movements that the disassembly keeps beside
the code that uses it. Three are functions: `DMARoutine`, which the game
copies into high RAM and runs there, and two that did not run and have a
few bytes that something read. For 1,616 of the 2,101 every byte of the
function was read, and of the three only `DMARoutine` is among those.

The 2,176 the game did neither with cannot be sorted by playing: the
disassembly has a function at 636 of them, the inside of one at 126, data at
788 and no telling label at 626.

## Lifted source

Lifted source includes `gblift.h` (`runtime/include/`, so it travels with every
generated project) and nothing else from the runtime.

```c
GBLIFT_FUNCTION(0x02, 0x4497, update_player_health) { ... }
```

declares `void update_player_health(GBContext* ctx)` and tells the tools which
subroutine it is. The rules:

- **On entry** `ctx` holds the registers as the original's first instruction
  would see them, with the caller's return address on the guest stack. Flags
  are `ctx->f_z`, `f_n`, `f_h`, `f_c`, each 0 or 1.
- **Memory** goes through `lift_read8` and `lift_write8`. That is what lets
  the checker see it.
- **Calling guest code**: `if (lift_call(ctx, address, return_address)) return;`.
  `return_address` is the address after the original `CALL`. A callee written
  in assembly can return past its caller (`pop af / ret`); then the result is
  nonzero and the function is over.
- **A call made by a jump**: `if (lift_call_pushed(ctx, address, return_address)) return;`
  where the function has pushed `return_address` itself and the code at
  `address` returns to it (`push de / jp hl`). It is `lift_call` without the
  push, and answers the same. Only for an address the function pushed: a
  jump with the caller's return address on top of the stack is `lift_jump`.
- **Ending**: `lift_ret(ctx)` then `return`. `lift_ret` pops an address and
  continues there, which is the caller unless the code pushed something else
  (`push hl / ret` is a jump). A jump out of the function is
  `lift_jump(ctx, address); return;`. `lift_reti` is `lift_ret` for `RETI`.
- **Time**: `lift_tick(ctx, cycles)` says an instruction has ended and how
  long it took. In the game that is where a pending interrupt is taken; its
  handler has run when `lift_tick` returns. `lift_advance(ctx, cycles)` passes
  time without that, for the cycles before a memory access inside one
  instruction, and before a call or a return, which hand the machine on
  themselves. Readable source may leave time out altogether: the checker
  reports "time not counted" and does not reject it, and in the game the
  function then takes no guest time at all.
- **Interrupts**: `lift_ei`, `lift_di`, and `lift_halt` (which includes the
  instruction's 4 cycles and returns once the CPU is awake again and the
  interrupt that woke it has been handled).
- **`ctx->pc`** is the function's to keep or not. In the game an interrupt
  taken inside lifted code returns to `ctx->pc`, and a save state records it.
  The mechanical version sets it before every `lift_tick`, which makes both
  exact. See [Save states](#save-states).
- The header also has the SM83's arithmetic with flags (`lift_add8`,
  `lift_rl`, `lift_daa`, ...) for code that needs them exact.

What the checker compares is everything, so a rewrite has to produce every
register and flag the original leaves, including ones no caller reads. Where a
value is known to be dead, name it: `--ignore fh,fn` for a whole run, or a
`--contract` file per function:

```json
{"functions": {"00:2881": {"ignore": ["fh", "fn"]}}}
```

The report records nothing about why an ignore is safe; that is the author's
claim. The export's `read_after` is the evidence to use, where it has any
(see [What a function owes](#what-a-function-owes)).

## What the tool writes

`gb_lift.py baseline` writes C in one of two styles. Both are checked like any
other lifted source, and both match on every function the checker can run.

**Mechanical** (the default): one group of statements per instruction, a
`goto` per jump, every flag computed, time and `ctx->pc` exact. A game built
with it ends in the same state as the original. It is the reference.

**Structured** (`--style structured`) is the one to read and edit:

- A flag is computed only where something still reads it. `dec b` whose flags
  nothing reads is `ctx->b--`; `and $7F` whose flags are read stays
  `lift_and8(ctx, 0x7F)`. Flags are always kept for a return, because the
  caller may read them. Before a call, and before a jump or a fall into
  another function, they are kept if that function needs them or does not
  always overwrite them.
- A conditional jump, return or call tests what the instruction that set its
  flag compared: `ctx->a == 0x05` after `cp 5`, `ctx->b != 0` after `dec b`,
  `ctx->a & 0x80` after `rrca`. That takes one instruction, and only one,
  whose write of the flag can reach the test. It need not be the instruction
  before, and something else may need the flags too, in which case they are
  computed as well.
- Where the registers that instruction compared have changed by the time of
  the test, or it compared with memory, the answer goes in a local:
  `carry = ctx->a < ctx->b;` before `sub b`, `zero = !(lift_read8(ctx,
  ctx->hl) & 0x80);` for `bit 7, [hl]`. The local is used where it lets the
  instruction drop its flags, and not otherwise.
- A flag a call returned, one the function was entered with, and one that
  two instructions may have set are tested as what they are: `if (ctx->f_c)`.
- Jumps become `if`, `do ... while` and `for (;;)` where the code is laid out
  that way. A jump that fits none of them stays a `goto`.
- Hardware registers are named (`rLY`). With symbols, so are RAM addresses and
  functions: export with `--symbols game.sym --symbol-policy names-only`
  (`names-only` so that the names do not change what the analysis finds), and
  the lifted function is `DisableLCD`, the address `wPlayerMoney`. The names
  are `#define`s in `lifted_names.h`, written next to the sources.

```c
/* gbfn:v1:0000:0061  DisableLCD  (12 instructions) */
/* takes nothing; of what it writes, the callers found read flags z n h c */
GBLIFT_FUNCTION(0x00, 0x0061, DisableLCD) {
    ctx->a = 0;  /* 0061: xor a */
    lift_write8(ctx, rIF, ctx->a);  /* 0062: ldh [$FF0F], a */
    ctx->a = lift_read8(ctx, rIE);  /* 0064: ldh a, [$FFFF] */
    ctx->b = ctx->a;  /* 0066: ld b, a */
    ctx->a = (uint8_t)(ctx->a & 0xFE);  /* 0067: res 0, a */
    lift_write8(ctx, rIE, ctx->a);  /* 0069: ldh [$FFFF], a */
    lift_step(ctx, 0x006B, 52);
    do {
        ctx->a = lift_read8(ctx, rLY);  /* 006B: ldh a, [$FF44] */
        /* 006D: cp $91 */
    } while (lift_branch(ctx, ctx->a != 0x91, 0x006B, 32, 0x0071, 28));  /* 006F: jr nz, $006B */
    ...
```

The second comment is the export's `reads` and, where every caller was found,
`read_after` (see [What a function owes](#what-a-function-owes)).

It is still a transliteration: registers are `ctx->a`, and one line is one
instruction. Giving values names and merging lines is the rewriting that is
left to a person or a model.

Structured source keeps time one of two ways (`--time`):

- `block` (the default): time is added up over each straight run of
  statements and passed at its end, by `lift_step` or inside `lift_branch`.
  The total is the original's. What moves is when an interrupt can arrive,
  by up to the length of the run, and so what a read of a free-running
  register returns. Played with the same input, Pokémon Red made the same
  542,288 lifted calls and ended at the same place with different random
  numbers.
- `exact`: every instruction ends with `lift_step`, and a game built with it
  ends in the same state as the original. The one thing that can differ at a
  moment in between is a flag nothing reads, which this style does not
  compute: stop the game inside a wait and the flags register may not be the
  original's, nor the copy of it an interrupt pushed.

`lift_step(ctx, next_pc, cycles)` sets `ctx->pc` and calls `lift_tick`.
`lift_branch(ctx, taken, target, taken_cycles, next_pc, cycles)` does the same
for a conditional jump and returns `taken`, so it can be the condition of an
`if` or a `while`.

## The checker

`gb_lift.py check` finds every `GBLIFT_FUNCTION` in the source given, builds
those files with the checker (`lift/checker/`), and runs it as one worker per
processor. For each function and each state it:

1. makes a machine state and copies it;
2. runs the original subroutine on one copy with an interpreter, until it
   returns to the caller;
3. runs the lifted C on the other copy;
4. compares registers, flags, the interrupt enable flag, every byte of RAM,
   the cartridge mapping, and the ordered list of writes to hardware registers
   and the mapper.

Bytes below the stack pointer are not compared (`--strict-stack` compares
them): they are pushes that were popped again.

### The machine

Both sides run on a Game Boy with the time taken out. Memory, the mapper and
the CPU are as on hardware. The devices do not run and no interrupt is
delivered. A write to a hardware register is recorded. A read of a register
the hardware changes on its own returns a value that depends on the state's
seed and on how many times that register has been read, so code that waits for
LY to reach a line ends on both sides after the same number of reads.

This makes the check independent of timing, which is the point: lifted source
does not have the original's timing. It also means the checker says nothing
about code whose correctness is its timing (a mid-frame raster effect).

**Waiting for an interrupt.** Most of a game waits: it sets a byte, executes
`HALT`, and loops until the frame's interrupt handler has cleared the byte.
With no handler that loop never ends, and neither does anything that calls
it, which in Pokémon Red was over a quarter of the real functions. So a `HALT`
stands for "a handler ran": it changes some of the bytes the code is
polling. Those are the bytes of RAM the code reads again after a `HALT`,
having read them before it and written them at neither time: a byte it keeps
coming back to look at, waiting for someone else to change it. Each such byte
changes at about every other `HALT` where there are a few, and two or so
change where there are many; to zero half the time, which ends "until it is
cleared", and to any value otherwise, which ends "until a button is down".
Hardware registers and the function's own stack are left alone.

The original runs first and decides the changes from the state's seed; the
lifted code is shown the same bytes changed at the same `HALT`s. So a rewrite
has to wait the way the original does: one that halts once and takes that
for the interrupt it wanted comes out with fewer `HALT`s and is a mismatch.
What this does not do is run the handler. A match that went through a wait
says the two agree whatever a handler does to what they poll, not that they
agree with the real one, and a loop that waits without a `HALT` (reading a
counter the handler decrements) still never ends.

**Code that does not return.** A main loop never returns, and a menu returns
only when the right button is down. They wait, though, every frame. So when
three quarters of a state's budget is gone (the instructions, the hardware
log, the changes `HALT`s may make) the original is stopped at its next
`HALT`, the lifted code is run to the same `HALT`, and the two are compared
there. What is compared is what an interrupt handler would find at that
moment: every byte of RAM, the stack and its pointer, whether interrupts are
on, the cartridge mapping, and the ordered list of hardware writes. The
registers and flags are not: until it returns they are the code's own
business, and a rewrite may keep them in locals. A function that returns
from no state at all is given 32 states and no more.

Callees are not lifted during a check: `lift_call` runs the original callee
from the ROM. Each function is checked against the original on its own, so one
wrong function cannot hide behind another.

### The states

The first states are random: registers, flags, all of RAM, the stored values
of hardware registers. Later ones are aimed: the checker records every address
the original read and changes those bytes in particular. A function's states
depend on the seed, on where the function is and on what it does, and on
nothing else in the export, so the same function is tried from the same
states when the export around it changes.

**The game's own memory.** Some code no memory made up from nothing gets
through: the frame's interrupt handler, which calls a routine the game
copied into RAM and a sound engine in a bank it reads from a variable, or a
decompressor, which wants compressed data. `--image FILE` adds states that
start from a moment of the game's memory, with made-up registers, and with
the bytes the function reads changed a few at a time as above:

```bash
./output/game/build/game --save-state-file logs/title.state   # play to a moment, then quit
python3 tools/gb_lift.py image logs/title.state -o logs/title.img
python3 tools/gb_lift.py check output/lift/game.json --rom game.gb --source output/lift/src \
    --image logs/title.img --image logs/overworld.img --json output/lift/report.json
```

The game writes the state when it stops; the ten slots of its menu write the
same kind of file. `check` runs every function from made-up memory and then
from each image, and gives one report: a function that differs anywhere is a
mismatch, one that passes anywhere and differs nowhere is a match, and
`passed_from` lists where it passed (`--images-only` leaves the made-up
states out). A handful of moments that differ is enough: of seven in Pokémon
Red, the title screen, the name entry and the first room gave all but 32 of
what the seven gave. `image --rom-bank` says which ROM bank a function of
the home bank is to find mapped; without it the checker tries several.

**The states a function was entered in.** An image has the game's memory
and made-up registers, so a function that wants a pointer in a register
gets the right memory and the wrong argument, and a copy whose made-up
pointer takes in the cartridge's registers is thrown away. A game played
with `--entry-states` keeps the registers and the whole of memory as they
were each time control arrived at the first instruction of a function, and
`check --entries` starts states from those:

```bash
python3 tools/gb_lift.py entries output/lift/game.json -o logs/functions.txt
./output/game/build/game --entry-states logs/played.entries --entry-states-of logs/functions.txt
python3 tools/gb_lift.py check output/lift/game.json --rom game.gb --source output/lift/src \
    --entries logs/played.entries --json output/lift/report.json
python3 tools/gb_lift.py entries output/lift/game.json --states logs/played.entries \
    --report output/lift/report.json
```

`entries -o` writes where each function of the export begins, one
`BANK:ADDRESS` a line (with `--report`, only the functions that report does
not call a match; never one whose code is in RAM or in a bank mapped low,
which cannot be lifted). The game runs in the interpreter while it keeps
the states, as it does for `--data-use`, and ends in the same state as
without. It plays as any game does: by hand, from a script (`--input`),
over a recorded run (`--replay-run`) or driven by a program (`--control`).
The file is written when the game stops.

Not every arrival is kept. A state is kept the first time a function is
arrived at with a return address not seen for it before, which is another
caller and likely another kind of use, and at its first, second, fourth,
eighth... arrival for half of what a function may have, which spreads them
over the run. A function may have eight (`--entry-states-per N`). Memory is
stored in pages of 256 bytes, and a page once however many states have it.
Without `--entry-states-of` the functions are the places a `CALL`, a `RST`
or an interrupt arrived at, which needs no export and finds no function
that is only ever jumped to.

`check --entries FILE` (repeatable, and `--images-only` leaves the made-up
states out as it does for images) runs each function from the states kept
of it and from nothing else: first each state exactly as it was, with its
registers, its memory, the banks that were mapped and its own return
address on the stack, and then each again with some of the bytes the
function reads changed, as states from an image are. A function no state
was kept of is not tried from the file. The report has, for each function,
`entry_states`: how many were `kept`, and of those run as they were how many
`matched`, `mismatched` and were `thrown_away`; a mismatch names the
`arrival` and says whether the state was `as_recorded`. A match from a state
as recorded says more than one from made-up memory, since the function did
there what the game asks of it: the C did what the ROM did in a moment the
game was really in.

A function is run until it is back at the return address the game had on
its stack, with the stack as it found it. Code lives at that address,
unlike the made-up one of the other states, and may run deeper down before
the function returns: a function entered from a helper that it calls
itself. That is not a return gone wrong. A function that does not come back
there (it reads what follows its call and returns past it) is not got
through by these states either.

`entries --states FILE...` says what the files cover: of the export's
functions, how many control arrived at and how many never, by how the
export came by each function and, with `--report`, by the checker's
verdict; `--json` lists the functions never arrived at. A function that a
long run of the whole game never arrives at is one the game seldom runs, or
no function.

On Pokémon Red three recorded plays were played again with the recorder
on: a run from a new game through the credits, and two scripted sessions
that use every item and every move and catch every Pokémon, 1,365,841
frames together. Each took about three minutes, and the two sessions met
every checkpoint of their logs with the recorder on, 842 of 842 and 742 of
742. They kept 19,546 states in 52.6 MB, about 2.7 KB a state. Control
arrived at the start of 2,107 of the export's 6,394 functions that can be
lifted, and never at 4,287: of the 2,939 the export calls `unreferenced`,
at two.

Checking every function again, from made-up memory, three images and the
three files, took 328 seconds. 1,663 of the 2,107 functions did as the
original from a state exactly as the game had it, and no lifted function
differed from any. What the states added to the verdicts is small: 4,671
functions matched before and 4,681 after, and 1,713 are left that the
checker cannot run. Of those 1,713 the game never arrived at 1,663, and the
disassembly has a function at 28 of them, data at 171 and no label at all
at 1,314: they are, nearly all, not functions. The other 50 were arrived at
and are still thrown away from every state: text and menus that wait for a
button, the start of the game, the script of a whole scene. What throws
most of a function's states away is, for 31 of them, more hardware writes
than the checker's log holds, for 13 the budget of instructions, and for 6
another bank found mapped under banked code, whose cause in a state the
game was really in has not been looked for.

The states did find something nobody was looking for. 19 functions that
matched from every made-up state and every image differed from a state the
game was in, each only while held to its signature (see
[Signatures](#signatures)): the line said a register was not taken, or was
kept, and on a path the made-up states had not found the function reads it
or changes it. With the files in the folder's `entries/`, `gb_lift.py
signatures` watched the functions from those states too and wrote 18 of the
19 lines again, and all 3,907 functions with a line then matched held to
it.

In a source folder, files in `entries/` named `*.entries` are checked from
as the images in `images/` are, by `build`, `signatures` and `convert`.

A state is thrown away, and counted, when the original does not get through
it:

| Reason | What happened |
|---|---|
| `timeout` | It ran more than `--max-steps` instructions, the last quarter of them without a `HALT` to be stopped at. |
| `log_overflow` | It made more than 4096 hardware writes and `HALT`s, the last quarter of them without a `HALT`: a copy whose made-up destination takes in the hardware registers. |
| `invalid_opcode` | It executed a byte that is no instruction. |
| `bad_return` | It reached the caller's return address with the stack out of place. |
| `another_bank_mapped` | Code in the switchable area ran with another ROM bank mapped than the one it is in: the function's own addresses, or what a call made from that area returned to. A helper restored a bank number it read from made-up memory, or a made-up pointer aimed a helper's write at the mapper. A byte that held the mapped bank in two such states is taken for the game's record of the bank and kept true in later states. |
| `bank_switched_under_code` | An instruction in the switchable area wrote the mapper and mapped another bank over itself. A game does not do that; a made-up pointer does. |
| `used_its_stack_as_data` | The original read or wrote, through a pointer, something it or its callees had pushed, the two sides came out differently, and the source did not push what the original pushed: it is source written by hand, and the state says nothing about it (see [Functions written by hand](#functions-written-by-hand)). This one is decided after the source has run. |

In the last two, what ran next was not the code that was lifted, or the code
the export describes, so nothing about it can be compared.

A function no state gets through is given up on after a dozen states and
reported `inconclusive`. It is given eight dozen once one of them failed over
the bank that was mapped, or where something is known to call it (any `kind`
but `unreferenced` and `shared_tail`): that is code, and what stops it is
mostly a made-up pointer, which another state will not have.

### Reading a result

```
match        gbfn:v1:0000:2881  lcd_off  500/500 states, 100% of its instructions reached  time not counted
match        gbfn:v1:0000:20af  delay_frame  500/500 states (500 through a wait), 100% of its instructions reached  same time
mismatch     gbfn:v1:0000:2881  lcd_off  0/500 states, 100% of its instructions reached
             state 5: a=01 bc=8D98 de=FFCE hl=C090 sp=DFEE z=1 n=0 h=0 c=1 bank=1B
               hardware write #1: original FF40=79, lifted FF40=78
```

- **match**: the two agreed on every state that could be used. It is evidence,
  not proof. The percentage is how much of the original those states executed;
  treat a match that reached 60% as a match on 60% of the function.
- **only from NAME.img**: with `--image`, nothing made up got the function
  through and the image did. It was compared from that moment of the game
  and near it, and from nowhere else. **only from NAME.entries** is the
  same of the states the function was entered in.
- **through a wait** (`waited` in the JSON): in that many of the states a
  `HALT` changed a byte the code was polling. Those states compare the two
  under the checker's stand-in for an interrupt
  (see [The machine](#the-machine)), not with a handler run.
- **compared at a wait, not returned** (`stopped_waiting`): in that many the
  original was still running when most of the budget was gone, and the two
  were compared at its next `HALT`, on memory, the stack and the hardware
  but not on registers. Where that is every state, the function never
  returned: what was checked is that it does the same things while it runs,
  as far as the percentage says.
- **mismatch**: at least one state differs. The first is printed with its
  entry registers; `--json` has up to three per function.
- **inconclusive**: no state could be used.
- **same time / time differs / time not counted**: whether the lifted code
  ticked the same number of cycles as the original. `cycles` in the JSON
  counts the states that agreed four ways: `equal`, `different`,
  `not_counted` (the source ticked nothing) and `not_compared` (the original
  used what it had pushed as data and the source does not push what the
  original pushes, so that the time says as little as a register would
  have: see [Functions written by hand](#functions-written-by-hand)).
  `time not compared` is what a function gets whose every state was one of
  those.

`--json FILE` writes the full report (`gbrecomp.lift-check`, version 1),
including the RAM addresses the original read, which is its real input list.

## In the game

`gb_lift.py package` copies lifted sources into a folder and writes
`lift.json`, the manifest: the ROM's hash and, for each function, its ID and
C name. With `--report` it takes only functions a `check --json` report says
matched (give several reports and a function has to match in one and mismatch
in none). `gbrecomp ROM --lift lift.json` then generates the game as usual and:

- copies the sources into the project's `lift/` folder and adds them to the
  build, with the runtime's side of `gblift.h` (`runtime/src/gbrt_lift.c`);
- emits, where the recompiled code reaches a lifted subroutine's first
  instruction, `if (gbrt_lift_enter(ctx, ...)) return;`. That one line is the
  whole hook. However the game gets to that address (a call, a jump, falling
  into it, the interpreter, an interrupt vector), the C runs;
- keeps the recompiled body behind the hook.

The generated executable has two more options:

| Option | Effect |
|---|---|
| `--no-lift` | Run every subroutine's recompiled body, as if nothing were lifted. |
| `--report-lift` | Print at exit how many lifted functions ran, how often lifted code ran guest code, how many frames ended inside it, and `lifted_share`: of the time the CPU was running (not waiting in HALT), how much passed in lifted code. |

`--no-lift` on the same executable is the comparison to make when a mod
misbehaves: it separates "my edit" from "the game".

### How lifted C shares the machine

Recompiled code returns to the scheduler between any two instructions and is
resumed by address. A C function cannot. But a subroutine that lifted code
calls may wait for the next frame, so lifted code runs on a coroutine:

- `lift_call` runs the callee on the coroutine with the scheduler's own loop
  (interrupts, HALT, the interpreter fallback and all), full speed, until it
  returns. If the frame ends meanwhile, the coroutine is left where it is, the
  main loop presents the frame and polls input as always, and the next
  `gb_run_cycles` goes back into the coroutine.
- A callee that is itself lifted is an ordinary nested C call on the same
  coroutine.
- `lift_call_pushed` is the same with the return address already on the
  guest stack. What tells the scheduler's loop that the callee is back is
  the program counter and the stack pointer together, checked against the
  machine every time, so a callee that does something else with that
  address only keeps the loop running.
- `lift_ret` and `lift_jump` only set the program counter: the lifted function
  returns and the dispatcher carries on from there.
- At `lift_tick` a pending interrupt is taken on the spot: the handler runs as
  guest code and returns into the C.

The coroutine is a Windows fiber. For Linux and macOS it is written with
`ucontext` and has not been built or run there yet.

### Save states

A save state holds the guest machine, not the C stack of a lifted function
that is half way through. Loading one drops the coroutine, and execution
carries on in recompiled code from `ctx->pc` and the return addresses on the
guest stack, which lead into the recompiled bodies. That is exact when the
machine at that moment is one the original could be in:

- For a mechanical lift it always is. It keeps `ctx->pc`, the stack and the
  registers as the original has them at every instruction.
- Hand-written source is exact at its calls if it keeps the stack the way the
  original does there (the checker does not verify this; it ignores bytes
  below the stack pointer). A state saved while such a function is inside
  `lift_tick` resumes at whatever `ctx->pc` holds: the function's start,
  unless the source sets it.

Until that is checked by a tool, treat save states made in the middle of a
rewritten function as unreliable; in-game saves are unaffected.

### What ran

A lifted function's C runs when the function is entered at its start. Entered
anywhere else, its recompiled code runs, and an edit to the C is not what
the game does there. That happens where the map has merged two functions
into one, and where a function is returned into after a jump through a
register that the export did not take for a call (see
[Calls made by hand](#calls-made-by-hand)). To see how much of a session
went which way, have the game log its dispatches and count them:

```bash
./output/game/build/game --trace-entries logs/game.trace
python3 tools/gb_lift.py coverage output/lift/game.json --trace logs/game.trace \
    --package output/lift/package/lift.json --report output/lift/check.json
```

```
675,900 dispatches at 413 addresses
   611,389 (90.5%) at the start of a lifted function: its C ran
     1,196 ( 0.2%) inside a lifted function, 1 of them: the recompiled code ran there, and an edit to the C is not what the game did
                       1,196  DisplayNamingScreen (01:6596) at 660B x577, 6612 x577, 65FC x13
    63,309 ( 9.4%) in 28 functions that are not lifted
                      29,705  UncompressSpriteDataLoop (00:2556) [shared_tail]  check: inconclusive (12 timeout)
         6 ( 0.0%) in no function of the export: code in RAM, or not decoded
```

The export and the package have to be the ones the game was built from.
With `--report` a function that is not lifted is listed with what the
checker said of it. The trace is a line a dispatch, about a megabyte and a
half for a thousand frames of Pokémon Red; `--trace` can be given more than
once. A function listed in the middle group that you mean to edit needs its
other way in dealt with first, and `edges --evidence` with a recording of
the same session does both kinds: an address a call arrives at becomes a
function of its own, and an address a callee returns to becomes a place the
function's C goes on from (see [Evidence from playing](#evidence-from-playing)).
The listing above is from before the second was done; with the evidence,
that group is empty.

### Other limits

- A callee that never returns (a soft reset reached through lifted code)
  leaves its callers' C frames on the coroutine. The game runs on correctly;
  the frames are only reclaimed by a state load or a reset from the menu.
- Single-stepping (`--differential`, a debugger) runs the recompiled bodies:
  it compares instruction by instruction, which lifted code does not have.
- `--lift` is for single-ROM desktop projects; not Android, not a multi-ROM
  launcher. The hub does not offer it yet.

## What it does on real games

All on Windows, 2026-10-09, `--pointer-scan strict`.

**Checker.** Every function lifted mechanically and checked at 500 random
states (Link's Awakening also from a save state). A mechanical lift that
mismatches is a bug in the lift, the interpreter or the checker, so zero is
the number that matters:

| Game | Mapper | Functions | Match | Mismatch | Inconclusive | `call` functions matched |
|---|---|---|---|---|---|---|
| The Legend of Zelda: Link's Awakening (USA, Europe, Rev 2) | MBC1 | 6,183 | 4,325 | 0 | 1,858 | 1,020 of 1,060 |
| Kirby's Dream Land (USA, Europe) | MBC1 | 1,842 | 819 | 0 | 1,023 | 195 of 244 |
| The Legend of Zelda: Oracle of Ages (USA, Australia), Game Boy Color | MBC5 | 14,997 | 10,366 | 0 | 4,631 | 2,489 of 2,710 |
| Pokémon Red (USA, Europe), with `--lift-edges` and three images | MBC3 | 5,844 | 3,808 | 0 | 2,036 | 1,116 of 1,168, 216 of 222 `far_call`, 175 of 175 `table_call` |

Export takes 4 to 10 seconds, building the checker 2 to 12, a run 4 to 44 on
20 cores. Pokémon Red's run from made-up memory is the 44, where it was 17
before waits ended: code that used to fill the log with `HALT`s at once now
runs on. Each image is a run more, 178 seconds for the four. Most
inconclusive functions are `unreferenced` code that floods the hardware log
or runs into an undefined opcode: data decoded as code.

Pokémon Red's row is from the checker as it is now, on an export made with
`--lift-edges` (see [Calls through helpers](#calls-through-helpers)), from
made-up memory and from three images of the game's own (see below); from
made-up memory alone it is 3,562. It has 19 functions fewer than it had,
and as many fewer matches, with the same instructions in functions that
match, 88,250 of them: a function that calls through a register by pushing
a return address is one function now, where it was two
(see [Calls made by hand](#calls-made-by-hand)). The
other three are from before the checker learnt to refuse a state in which
banked code runs with the wrong bank mapped (see [The states](#the-states))
and before it could get through a wait for an interrupt
(see [The machine](#the-machine)). They have not been run again since, and
have not had their far calls looked for; they count some functions that are
data and only "matched" by running another bank's bytes on both sides, and
none of the functions that wait. In Pokémon Red, against its disassembly,
the bank rule took 154 such functions out of the matches, and learning the
bank variable properly brought 78 functions in. Without `--lift-edges` the
same ROM gives 5,648 functions and 3,361 matches.

**Waits.** The export as it was then, of 5,854 functions, gave 2,446 matches
before a `HALT` stood for a handler having run, and 3,141 once it did: in a
run that tried all 500 states of every function, 571 of the 704 functions at
a function label that passed none had failed most often in a wait that never
ended. Comparing at a wait the states that had not returned brought in 395
more, and giving a function something calls eight dozen states before giving
up on it another 35; neither took one out, and none mismatches. Of the
3,578 matches that export then had, 1,800 go through a wait in some state,
1,470 have a state that was compared at a wait, and 350 returned from none. Against the disassembly,
of the 2,007 functions at a function label in the export made with the
evidence below, 1,833 pass (91%); before any of this it was 1,242 of 1,996
(62%).

Of the 174 that still pass no state, 117 fill the hardware log, nearly all
of them with writes into the cartridge's address space: a copy or a fill
running through a pointer out of made-up memory. 23 run out of instructions
without a `HALT` to stop at, among them the cartridge's entry point and the
loops that decompress a picture. The other 34 fall to the bank rules, an
undefined opcode or a return with the stack out of place. Such functions do
pass in some states (the party menu in one of twenty), so what they need is
more states or better ones, not bigger budgets: a million instructions a
state instead of 200,000 passed 10 more and doubled the run, and a log four
times the size 5. Eight dozen states for any function once one of its
states has run a copy into the cartridge passes 24 more of them, and 172
that are not at a function label, for twice the run; that is not done.

**The game's own memory.** Three images of the game (see
[The states](#the-states)): the title screen, the name entry, and the first
room, which are frames 700, 3,300 and 39,000 of the scripted run. Checked
from them as well, 246 more functions pass, 40 of them at a function label,
and none mismatches: 3,808 of 5,844. They are the ones the game was running
without: the second half of the frame's interrupt handler, which passes in
16, 32 and 5 states from the three images and in none made up, the loops of
the picture decompressor, the copy of rows to the screen. Of the
functions at a function label 1,868 of 2,002 pass (93%). Four more images
from other moments of the same run added 32 functions, one of them at a
function label, when that was tried. Of the 134 at a function label that still pass nothing, 87
fill the hardware log, 17 run out of instructions and 30 fall to the bank
rules, an undefined opcode or the stack.

**Calls into another bank by hand.** 39 direct calls and jumps from the home
bank had no bank in the analysis. For 20 of them the bank is written out
right before, `ld a, n` / `ld [$2000], a`, and the analysis had lost it
because something was thought to jump into the middle of that; the analyzer
now reads it from those bytes when the ways in do not agree. The other 19
have no such bytes before them. 20 more functions are `call` functions with
a caller, 9 more of the disassembly's functions are functions in the export,
and generated code calls those targets directly, behind its check of the
mapped bank, where it handed them to the dispatcher. A game built without
lifted code ends the run below in the same state as before.

**Calls through helpers.** `gb_lift.py edges` on Pokémon Red takes 5 seconds:
3,842 calls and jumps with a constant in some register, to 619 functions the
export cannot follow.

- 638 go to one place whatever else is in the machine: 245 through the
  helper that takes a bank in B and an address in HL, to 177 functions; 364
  through the one that takes a number in A and looks it up in a table in
  another bank, to 81; the rest through five smaller ones.
- 54 go through a table that ends where code begins, 233 entries in all: 26
  through the helper that calls entry A of the table in HL, 28 through the
  one that runs a map's current script. Against the disassembly every one
  of the 233 is code. 42 more go through a table whose end nothing marks
  and 6 through one that does not start at a constant of the call; those
  are not read.
- 489 go to one place with or without the constants (the function called
  makes the far call), and 161 go to different places.

That is 446 places. Of the 444 the first round found, 236 were functions in
the export already, 165 were inside another function and 43 had not been
decoded; the second round found two more and the third nothing. Against the
disassembly, 221 of the 222 `far_call` functions and all 175 `table_call`
functions are at a function label. The functions reached from the entry
point by what the export lists go from 54 to 1,116.

**Evidence from playing.** Pokémon Red built with those edges and played
headless with `--evidence`: once with the 3,600-frame script of the runs
below, and once for 40,000 frames with buttons pressed blind, which gets
through Professor Oak's speech and both naming screens into the player's
bedroom.

- Recording costs about 1% unpaced (3,450 frames per second against 3,498
  without, the best of three runs of the 40,000 frames), and the game ends
  in the same state.
- The short run made 28 different jumps its code does not spell out. 13 came
  from a call `edges` had the ROM's answer for, and all 13 went where the ROM
  said.
- The long run made 60. 20 came from such a call, and went where the ROM
  said. The others gave 31 edges the ROM does not: 29 on the call that
  entered the function the jump happened in, 2 on the jump itself. They go
  to 17 places. For 10 of those the export had no caller, 8 being
  `unreferenced` and 2 not functions at all: the seven button handlers of
  the naming screen, the script of the bedroom's map (`RedsHouse2F_Script`,
  whose address the map's header puts in RAM), `PrintRedSNESText` and the
  first audio bank's music update. In the disassembly 3 of the 10 are at a
  function label and 7 at a local label.
- A call, an interrupt or such a jump arrived at 255 addresses in that run.
  With the edges, two of them were still not the start of a function, each
  a function the map had merged into data that decodes before it:
  `Audio3_UpdateMusic`, which the frame's interrupt calls in a bank it reads
  from a variable, and `CopyDebugName`. They are functions now, 12 of the
  kind `seen` in all, and all 255 are among the export's 5,852. The functions reached from the entry
  point by what the export lists go from 1,112 to 1,154.
- The naming screen's jump to a button's handler was made with two
  different addresses pushed for the handler to return to. Both are in the
  naming screen's own code, and its lifted C goes on from them
  (see [Calls made by hand](#calls-made-by-hand)); a third the table has,
  for the Select button, the run did not press.
- With those edges the checker matches 3,566 functions from made-up memory
  and 3,812 with the three images, the 12 `seen` ones among them, and
  mismatches none, in the mechanical style and in the structured one with
  either `--time`. Built in, the game ends 3,600 frames in the same state as
  with `--no-lift`, in 645,343 lifted calls (for the structured style see
  below).

That is 4 more than without the evidence, where the two exports differ in 6
functions. Every one of the 5,847 they share has the same verdict in both,
because a function is tried from the same states whatever else the export
holds.

**In the game.** The matched functions packaged, built in with `--lift`, and
the one executable played headless with a scripted input twice, with the
lifted code and with `--no-lift`. Compared: every register including PC, the
cycle counters, all of work RAM and high RAM (`--dump-state`).

| Game | Lifted functions | Frames | Lifted calls | Deepest nesting | State |
|---|---|---|---|---|---|
| Link's Awakening | 4,185 | 5,000 | 367,525 | 6 | identical |
| Pokémon Red, with `--lift-edges` and three images | 3,808 | 3,600 | 644,543 | 17 | identical |
| Kirby's Dream Land | 819 | 3,600 | 301,982 | 8 | identical |
| Oracle of Ages | 10,366 | 3,600 | 71,741 | 8 | identical |

Pokémon Red's row is from the current checker and generators; the other
three were not run again (see the note under the table above). Built with
`--lift-edges` and nothing lifted, Pokémon Red ends in the same state as
built without, with 3 dispatches to the interpreter over the run where
there were 6.

Speed, headless and unpaced, best of five 3,600-frame runs, without and with
the lifted code: Pokémon Red 3,550 and 3,829 frames per second (the build
from before calls by hand were kept whole measured 3,563 and 3,841 in the
same sitting), and from
the earlier runs Link's Awakening 2,754 and 2,469, Oracle of Ages 2,057 and
2,580. Generating a project takes 2 to 18 seconds and building it 9 to 49.
Pokémon Red's script gets from power-on through the title, a new game and
Professor Oak's speech to the name entry screen; frames captured from the
lifted run show the picture intact.

**How much of the game is lifted.** With those functions built in, 97.4% of
the CPU's running time in Pokémon Red passes in lifted code (`lifted_share`,
over the run above), and 97.9% with the evidence. It was 89.3% before the
functions that only the game's own memory gets through were checked from
it, 84.2% before the calls into another bank by hand were bound, 81.1%
before the functions that do not return were compared at a wait, 79.5%
before the ones that wait could be checked at all and 74.0% before its far
calls were followed, and is 74.0% in Link's Awakening. Lifted code hands the
frame back 3,621 times in the run's 3,600 frames, where it did 1,746 times:
nearly every frame now ends inside it. The functions its tables add are map
scripts, which that run, ending at the name entry screen, does not reach.

What the rest is, from `gb_lift.py coverage` on the run's dispatches
(see [What ran](#what-ran)): of 652,563, 640,357 (98.1%) are the start of a
lifted function, whose C then runs. There were 687,623 before a call made
by hand was a call: where its callee returns is no longer somewhere the
game has to be sent. 4,436 land inside a lifted function, where the
recompiled code runs and an edit to the C would not show. Before the calls
into another bank were bound that was 35,350, `GetPredefPointer` alone
27,205 of them, inside data that decodes before it. Of the 4,436, 3,235 are
in `Audio3_UpdateMusic`, which the evidence makes a function, and 1,196 are
the naming screen going on after a button's handler has returned to it,
which the evidence makes a call: with the evidence none is left, of 652,553
dispatches, and 644,783 (98.8%) are the start of a lifted function. 7,764
are in 10 functions that are not lifted, 7,192 of them the routine in high
RAM that starts the sprite transfer, which is not in the ROM to be lifted.
Before the images it was 63,309 in 28.

**The structured style.** The functions of the export made with the evidence
written with `--style structured`, in both `--time` modes, checked at 500
states from made-up memory and each image: the same 3,812 match, none
mismatch, and every one counts the same
total time as the original. Built into the game with `--time exact` and the
disassembly's names, it ends 3,600 frames with every register, counter and
byte of RAM as the original has them, except the flags and one byte below
the stack pointer. The run stops in the frame wait. Its caller is lifted now,
and does not compute flags the wait is about to overwrite; the interrupt that
ended the frame pushed them. The source is 184,634 lines against 435,382
mechanical, with 2,785 `if`s, 640 `do` loops, 297 `for (;;)` loops and 2,352
`goto`s left. 16 jumps through HL are written as calls, one of them behind a
look at the address on the stack.

In those 3,812 functions there are 6,238 conditional jumps, returns and
calls. 4,417 are written as a comparison, 280 of them through a local; before
a test was traced to the instruction that set its flag, wherever that is,
1,260 were. Of the 1,821 that test a flag, 437 test one the function was
entered with and 382 one a call returned, which is what they should say; 93
test one that two instructions may have set; and for 909 the one instruction
that set it is a kind this does not write out (a rotate, a subtraction's
borrow) or its flags are needed elsewhere as well.

6,931 instructions still compute flags. For 1,948 of them the only reader is
a return: a caller may read them, and which callers do is what the export
mostly cannot say (995 of the 2,431 `call` and `shared_tail` functions have
no sign of another way in, and for 711 of those the callers found read every
flag anyway, because a caller that is itself entered from who knows where
hands everything on). 1,760 feed arithmetic that takes a flag in (`ADC`,
`RLA`, `PUSH AF`), 1,310 are computed because the function runs on into code
that may read them, and 777 because a callee does.

**A mod.** Pokémon Red's player-data setup (`03:7850`) writes the starting
money, 3,000. Three lines added to its lifted C in the generated project's
`lift/` folder, `ninja` (1 second: one file recompiles), and a new game starts
with 999,999; `--no-lift` on that executable still starts with 3,000.

**The source folder.** Pokémon Red's folder
(see [Working in the source](#working-in-the-source)), 2026-10-10, from an
export made with `--pointer-scan strict`, `--lift-edges` and annotations of
what a played game showed to be data (`DATA_FILES.md`), with the
disassembly's symbol file brought as `--symbols`. Made in 6 seconds: 6,243
functions in 166,607 lines and 32 files, 2,736 of them under a name.

- *Built untouched*, with three images in `images/`: 4,445 functions match
  and are built in, none mismatches, 1,798 the checker could not run. The
  check took 228 seconds on a machine doing other work (62 from made-up
  memory alone on a quiet one), generating 12, compiling 33. Played with the
  same buttons at random for 3,600 frames from a fresh boot and from five
  save states of a play-through, the first Pokémon to the fourth gym, the
  game ends
  in the same state as the one generated with no lifted code, flags and what
  an interrupt left below the stack pointer aside, with 97.9% to 98.6% of
  the CPU's running time in lifted code.
- *A comment* above `DisableLCD` and `EnableLCD` *tidied* (a named constant,
  a statement a line): `build` checks those two, 0.9 seconds, both match,
  and compiles in 0.9; 4 seconds from the command to the game.
- *A change*: the function that shows the copyright screen loads palette
  `0x1B` where the game loads `0xE4`. `build` stops in 5 seconds, on
  `hardware write #80: original FF47=E4, lifted FF47=1B`, with nothing
  built. With a line in `changes.txt` it builds in 3 seconds, the game not
  generated again: the screen is in negative two seconds after power-on,
  `--no-lift` shows it as it was, and the game still ends 3,600 frames in
  the same state from all six starts.
- *Updated* from an export made with fuller annotations, in 4 seconds: the
  3 changed functions kept, 6,023 untouched ones refreshed (222 of them
  written differently), 231 new, 217 untouched ones gone, nothing left
  unreconciled. `build` then checks 453 functions in 15 seconds, generates
  in 19 and compiles in 20; 4,469 functions are built in, the change among
  them, and the six runs end the same.
- *With a comment on every function*, as a pass that describes them all
  would leave the first folder, the same update keeps 6,026, refreshes
  none, adds 231, and cannot reconcile 417: 217 are no longer functions and
  go to `unplaced/`, and 200 are kept though their code is not what it was.
  Of those 200 the checker then matches 90, cannot run 110, and mismatches
  none. That was before comments were carried: such a function was then
  kept as it was. It is now written afresh where its comments have a place.
- *Comments carried* (2026-10-11). A folder of the game in which nine
  functions of the home bank have comments, 40 lines of them, above the
  functions and over statements, made by the tool as it was before it
  carried any. Updated with nothing changed, 4 seconds: the nine are found
  to be only commented, none is rewritten, and the file is the same byte
  for byte. Two names added to `names.sym`, for a function one of the nine
  calls and for one of the nine itself: 4 seconds, and those two are
  written afresh with the names, every comment where it stood. Switched to
  `--time block`, 4 seconds: all nine follow, a comment that stood over a
  loop now between the line that passes the time before the loop and the
  `do {`. The folder is 165,913 lines and 8.6 megabytes that way, 166,647
  and 11.7 with exact time: the lines are as many and shorter. `build`
  checks the 6,011 functions that came out differently, in 203 seconds
  with three images, and matches the 4,445 it matched, the nine among them.
  The game is then where that style leaves it
  (see [What the tool writes](#what-the-tool-writes)): 3,600 frames from a
  fresh boot it differs from the game with no lifted code in two registers,
  the program counter (three bytes on, in the same wait), 60 cycles, the
  two bytes of random numbers, the player's number that is made from them,
  and 21 bytes left below the stack pointer. Switched back, the nine are word for
  word what they were, and the game ends in the same state as the one with
  no lifted code from the fresh boot and the five save states.

## Scoring against a disassembly

Where a community disassembly of the same ROM exists, it is ground truth the
recompiler never had. `gb_lift.py score` reads its RGBDS `.sym` file for where
every label is, and, given the source tree, tells code labels from data labels
by what follows them:

```bash
python3 tools/gb_lift.py score output/lift/game.json --sym pokered.sym --source pokered/
```

Score an export made without `--symbols`. Pokémon Red (USA, Europe) against
pret/pokered, `--pointer-scan strict` and `--lift-edges`, 2026-10-09:

| Our functions, by `kind` | Total | At a function label | At a local label | At a data label | At no label |
|---|---|---|---|---|---|
| `call` | 1,168 | 987 | 76 | 3 | 102 |
| `far_call` | 222 | 221 | 0 | 0 | 1 |
| `table_call` | 175 | 175 | 0 | 0 | 0 |
| `shared_tail` | 1,263 | 174 | 143 | 17 | 929 |
| `unreferenced` | 2,996 | 443 | 44 | 440 | 2,069 |

- Of the disassembly's 2,318 functions, 1,711 (74%) are functions in the
  export, 488 (21%) are decoded but sit inside another of our functions, and
  119 (5%) were not decoded. It was 1,712 before calls made by hand were
  kept whole: `_RunPaletteCommand` pushes the address of `SendSGBPackets`
  for the routine it jumps to to return into, nothing else in the decoded
  code names that address, and so it is now where `_RunPaletteCommand` goes
  on, as the code has it, and not a function. Before the calls into another
  bank by hand were bound it was 1,703, 494 and 121; with the far calls and no tables 1,603
  (69%), 581 and 134; with neither, 1,487 (64%), 662 and 169.
- Of 127,666 bytes of real code the export decoded 89%. Of 505,251 bytes of
  data it decoded 16% as code.

So a `call`, `far_call` or `table_call` function can be trusted,
`unreferenced` ones are mostly data that happens to decode, and a fifth of
the real functions are not yet separate functions in the map: they are
reached through tables whose end nothing marks or through addresses kept in
RAM, and end up merged into a neighbour.

That fifth is what [declared tables](#tables-somebody-has-read) are for.
With 49 of them (2026-10-10), all 376 `declared_table` functions are at a
function label, and 2,041 of the disassembly's 2,318 functions (88%) are
functions of the export with nothing played, 2,088 (90%) with played
evidence as well, 218 inside another function and 12 not decoded. Of the
230, 186 are labels the disassembly's own source only ever jumps to.

**Pieces that are only jumped to.** Which of them is a function is the
disassembler's choice, and no rule gets all of it back: where a block is
reached was tried every way against that choice. A block the function runs
into as well is a function there 21 times and a local label 1,986 times. One
that follows on from the function and is reached by `JR` only, 43 and 1,098;
by `JP` only, 44 and 53. But a stretch that does not follow on from any
block of its function, reached by `JP` only: 64 and 11, and reached by `JR`,
14 and 39. So that one is cut out (`detached_tail`), and the next export
cut that way is cut again until nothing changes. With it 2,159 of the 2,318
(93%) are functions of the export, 148 inside another and 11 not decoded.
Of the 91 `detached_tail` functions 77 are at a function label, 11 at a
local label (`ItemUseMedicine.useRareCandy`, `Audio1_PlaySound.playSoundCommon`:
stretches as long as a routine) and 3 at none. The checker matches 4,459
functions of that export and mismatches none, and the game built with them
ends 3,600 frames in the same state as with `--no-lift`.
Functions made of data never run and never pass the checker, so they do no
harm; the merged ones are the loss.

## Tests

- `lift_checker_cpu_matches_runtime_interpreter`: the checker's interpreter
  against the runtime's, every opcode, 200 states each: registers, flags,
  memory and cycle count. Also that it refuses a state in which banked code
  maps another bank over itself, or is returned to with another bank mapped,
  and accepts the same writes from the home bank.
- `lift_export_baseline_and_checker_end_to_end`: a ROM with a function for
  every instruction. Checks the export's facts (what a function needs, a
  stack that is not balanced, what a function's one caller reads after it),
  that every mechanical lift matches with full coverage and exact time, that
  the structured style matches with the same time in both `--time` modes,
  that its tests are written from the instruction that set the flag where
  they can be and as the flag where they cannot, that `score` counts a small
  disassembly right, that a wrong edit is caught, and that a readable rewrite
  is accepted. Two functions call through HL with a return address they push:
  the export has to keep each whole, their lifted C has to make the call and
  go on, a flag tested after it has to be the callee's, and an edit to what
  follows the call has to be caught. Two functions jump by `JP` to a stretch
  that does not follow on from them, one of them with something pushed that
  the stretch pops: each stretch has to be a `detached_tail` function with
  the one caller and a balanced stack, and the same stretch reached by `JR`,
  or following on from its function, has to stay part of it. Two of its functions wait for an interrupt: they have to
  match, in every state through a wait; a rewrite that waits in a loop has to
  be accepted, and one that halts once and goes on has to be a mismatch. A
  third is a main loop that never returns: it has to match, every state
  compared at a wait and 32 of them enough; a rewrite of it that keeps
  nothing in the registers has to be accepted, and one that counts two a
  frame has to be a mismatch on the byte it counts in.
- `lift_far_call_edges_end_to_end`: a four-bank ROM whose banked code is
  reached only through two helpers: one takes the bank in B and the address
  in HL, the other a table in HL and a number in A. The first export has to
  record the constants at each call, one of them loaded in an earlier block.
  `edges` has to find each far call's target, leave out the call whose bank
  is not a constant, and put a wrapper's far call on the wrapper. It has to
  read a table that ends at its own first function and one that ends at a
  function something calls, the second only once the far call to the code
  that uses it has been found, and leave alone a table with nothing to mark
  its end. Looking and exporting has to settle; the export it settles on has
  to have every target as a `far_call` or `table_call` function with its
  callers; those functions have to lift and match; a project generated with
  the edges has to compile them; and edges from another ROM have to be
  refused. Then a second ROM (`tests/test_lift_tables.py`) whose banked
  functions are named only by tables nothing can be followed through: a
  table of pointers to records, each in the bank a second table gives, with
  the function's address five bytes in; a list of three-byte entries that
  ends with a byte and has an entry that is none; lists of records that name
  their own bank; and a plain table a helper calls through, with nothing to
  mark its end. `edges --unread` has to write that last one out to be
  finished and refuse it back unfinished. `edges --tables` has to read them
  all; looking and exporting has to settle on an export where each function
  is a `declared_table` that says which entry of which table it is, one of
  them cut out of the function that ran into it, and the plain table's are
  `table_call` on the call; they have to lift and match and be compiled
  only when the edges are given; and declarations that cannot be right
  (past the end, at what is no code, the bank unsaid, a field misspelt) and
  an edges file with a declared table outside the ROM have to be refused.
- `generated_lift_entries_end_to_end`: a small game with a function that
  returns only when a register points at a record its caller made. The game
  run with `--entry-states` has to end as it does without and as it does
  with `--data-use`, alone and with both; the file has to hold the
  registers, the return address and the memory of exactly the arrivals the
  rule keeps, by caller and by count, and no more of a function than
  `--entry-states-per` allows; with no list the functions have to be where
  calls and the interrupt arrived. `check --entries` has to get the
  function through, each state as the game had it, where made-up states
  and an image get it through none; a function entered from a helper that
  it calls itself has to be run to its own return; an edit to the lifted C
  has to be caught from the first state as recorded; a function no state
  was kept of is not tried. `entries` has to write the list and count what
  a file covers, and `build` has to give the function its verdict from a
  file in a source folder's `entries/`. A lifted game asked for the states
  runs its recompiled code.
- `generated_executed_map_end_to_end`: a small game in which the analysis
  is told to decode a table the game reads, and two bytes it does nothing
  with, as functions. Run with `--executed` it has to end as it does
  without, alone and beside `--data-use`, whose record has to be the same
  with the map kept beside it; the map has to have exactly the bytes of the
  instructions that ran, first bytes apart from later ones, and nothing of
  the data, the header or a function that never ran. `gb_lift.py executed`
  has to sort the functions: the table as read and never executed, the two
  bytes and the function that never ran as neither, code reached through a
  register as code the export does not have; without a record of what was
  read the table is neither; a function whose first instruction did not
  run, and one whose bytes ran only as other instructions, are told apart
  on maps made by hand; maps add up, and a map of another ROM is refused.
- `generated_evidence_end_to_end`: a small game that reaches code through
  addresses it keeps in RAM (which no instruction names, so the analysis
  does not find it), with `push hl / ret`, and by pushing a return address
  by hand before a `JP HL`. Run with `--evidence` it has to end in the same
  state and record exactly those jumps, each with the call that entered the
  function, and no ordinary call, return or interrupt; recorded from a save
  state made with a call in progress, it has to hold the same and no more.
  `edges --evidence` has to turn them into edges; the export made with them has to have the
  functions as `seen`, with their caller, `reaches_seen` and `ran`; a game
  generated with them has to compile what the first one interpreted and end
  in the same state; and evidence from another ROM has to be refused. An
  arrival added to the record in the middle of a function has to be
  reported by `edges`, be a `seen` function in the next export, and settle.
  One function makes four calls by hand with the callee and the place to
  come back to both read from a table, as Pokémon Red's naming screen does:
  the export made with the evidence has to list the two places, cut out of
  the middle of a block, and keep them the function's own; the checker has
  to pass its lifted C, having run every instruction, and catch an edit to
  what follows a return; and the game built with that C has to end in the
  same state with no dispatch into the middle of a lifted function.
- `generated_lifted_source_end_to_end`: a small game whose subroutines call
  lifted and plain code, wait for frames with HALT, get interrupted, return
  past their caller and jump with `push hl / ret`. Lifted, packaged, generated
  with `--lift` and built, it has to end 240 frames in the same state as with
  `--no-lift`, cycle for cycle, in the mechanical style and in the structured
  style with exact time; resume identically from a save state made inside
  lifted code; and change when one lifted function is edited and the project
  rebuilt. One of them calls through HL with a return address it pushes:
  the edit that is checked includes the code that call comes back to.
  Asked for evidence, it has to run its recompiled bodies and
  record from them. `coverage` on its dispatches has to count the one jump
  into the middle of a lifted function, as many times as it was made. One of
  its functions returns only once the game's start-up has run: `check` has
  to find it inconclusive from made-up memory, and a match from an image
  made of the game's save state, and say that is where it passed.
- `generated_explore_end_to_end`: a small game with a room to cross, 200
  pixels at a pixel every fourth frame, and at the far side a jump through
  an address in RAM to a function no instruction names. Looking for new
  code only, `explore` has to keep nothing on the way and not get there;
  keeping the screens it has to cross, write an evidence file with that
  jump and its target and nothing else, leave no save beside the game, go
  on from where it stopped when run again, start from a save state it is
  handed, keep the first moment past each of two checkpoints, still walk
  when started from a state made after the game's 32-bit cycle count has
  wrapped (buttons are timed by the count that does not), and give
  `edges --evidence` what makes the function a function.
- `analyzer_resolves_home_bank_calls`: a call into another bank made by hand
  has to be bound to the bank written right before it when another way in
  arrives with another bank, and left to the dispatcher when the value
  written is not the one loaded.
- `generated_lift_source_folder_end_to_end`: a small game's source folder
  (see [Working in the source](#working-in-the-source)), made from an export
  with a symbol file. Made again, no file may be touched. Before any game is
  built the folder has to configure and compile by itself. The game `build`
  makes from it untouched has to end as with `--no-lift`, and its project
  may hold no copy of the sources. Comments on some functions and a body
  tidied in another have to be kept and still match, with only the
  functions that changed checked again. A function changed on purpose
  has to stop the build, the game left as it was, until `changes.txt` names
  it (naming another function does not do), and the game then has to end
  differently by just that change, without being generated again. An edit
  built with `cmake --build` alone, as an editor builds, has to be in the
  game. Updated from a second export of the ROM, in which one function is
  gone, two are more code than they were and one is less: the functions
  whose code was changed have to be kept, text and name, and so do one with
  a name given where it stands, one with a comment at the end of a
  statement, one with a comment that looks like the tool's and one with a
  line of the tool's taken out. The ones only commented have to be written
  afresh: one that has a comment under the tool's lines, over a statement,
  over a loop, inside it, after a blank line and at the end has to come out
  with a name `names.sym` was given meanwhile and every comment where it
  stood; one that is more code than it was, with its comment over the same
  instruction; one whose comment begins like the tool's `takes` line; and
  one the tool has nothing new for has to stay word for word. One whose
  comment is about an instruction it no longer has has to be left exactly
  as it is and named, at this update and the next. The untouched ones have
  to be written afresh; the function with no place put aside with its
  comment; the one whose changed code is no longer what it was written from
  kept, named and checked again; a file saved with other line endings not
  taken for changed; a function taken out by hand left out until
  `--restore` asks for it, and then put back where its address belongs; a
  name written into the tool's header by hand kept. Switched to `--time
  block`, the commented functions have to follow with their comments, the
  changed ones stay, and the game still match and run; switched back, the
  file has to be the one it was, byte for byte. On text made up in the
  test, a comment has to follow its statement out of a loop the tool no
  longer writes, and comments must not be placed where their instruction is
  written twice. A function in the folder twice, a folder of another ROM
  and a folder that is somebody else's have to be refused.

## Not done

- **The hub.** Lifting is a command-line workflow; the hub neither makes
  packages nor passes `--lift`.
- **Checking what save states need.** Whether a rewritten function keeps the
  stack and `ctx->pc` as the original has them at its calls is not verified
  (see [Save states](#save-states)).
- **Linux and macOS.** The coroutine code for them is written and has not
  been built or run.
- **The rest of readability.** The structured style stops at statements:
  no `else`, no `break`, no named values in place of registers, no merging of
  a load and the test that follows it into one expression.
- **Signatures.** What a function needs on entry is known
  (`needs`, `always_writes`). What its callers read after it is known only
  where every caller is, and in a banked game that is rare: most code is
  reached through bank-switching helpers and tables, so the list of callers
  cannot be shown complete and `read_after` is "everything" for most
  functions (see [What it does on real games](#what-it-does-on-real-games)).
  So every flag and register is still kept at a return. Dropping them takes
  the missing call edges (following far calls, evidence from running the
  game), or a check when the function is entered that the caller is a listed
  one, with the recompiled body for any other. A saved register is not yet
  told from a used one either.
- **A better function map.** With its far calls followed, its tables read
  and 49 more declared (see [Tables somebody has read](#tables-somebody-has-read)),
  a tenth of Pokémon Red's real functions are still merged into a neighbour
  and a sixth of its data is decoded as code (see
  [Scoring against a disassembly](#scoring-against-a-disassembly)). What is
  left of the tenth is mostly pieces that are only ever jumped to. The ones
  that lie apart from their function are cut out (`detached_tail`); the
  rest, about 120, are labels in a run of code that nothing in the code
  tells from a local label. And the declarations were written with the
  disassembly open, to look up where each table is. Writing them for a game
  that has none, from the lifted C of the routine that uses the table, is
  the reading a model is meant to do, and has not been done.
- **Text that runs code.** A pointer to text whose first byte says "what
  follows is code" names a function too, one byte on. A declaration cannot
  say that yet.
- **Evidence from a played game.** The recorded runs so far are scripted and
  headless. A session played by a person, across maps, battles and menus, is
  what would fill in the scripts and the tables above, and has not been
  recorded.
- **Interrupts and timing.** The checker keeps no time and runs no handler.
  A `HALT` stands in for one (see [The machine](#the-machine)), which says a
  rewrite waits as the original does, not that the two agree with the game's
  own handler. A loop that waits without a `HALT` never ends there.
- **What still passes no state.** A tenth of Pokémon Red's real functions
  (see [What it does on real games](#what-it-does-on-real-games)), most of
  them stopped by a copy or a fill running through a pointer out of made-up
  memory into the cartridge. They pass in a state in ten or twenty; making
  one aimed byte in eight the high half of a pointer into RAM did not raise
  that. What would is states from the game itself: the registers and memory
  a function was really entered with, recorded while it is played.
- **Code a lifted function does not run.** Entered anywhere but at its
  start, a function runs its recompiled code, and an edit to its C is not
  what the game does (`gb_lift.py coverage` counts it, see
  [What ran](#what-ran)). Where the map has merged a function into a
  neighbour, a call that arrives there in a recorded run makes it a
  function, and where a function is returned into after a jump through a
  register, a run that saw it makes that a call
  (see [Calls made by hand](#calls-made-by-hand)). Both are known only
  where a run went: a place to come back to that is read from a table is
  not looked for in the ROM, though the table is there to be read, and a
  call whose bank comes out of a variable is found only once it is made.
  A `RET` used as the jump (`push de / push hl / ret`) is not taken for a
  call either.
- **The function map is not pruned.** `gb_lift.py executed` says which of
  the export's functions a played game read as data and never executed
  (see [What the game ran](#what-the-game-ran)), and nothing gives that
  back to the analysis: the export still has them, the source folder still
  has their C, and their bytes are still code to a data tree. The rule to
  give back is not written either: a function every byte of which was read
  is data but for code a game copies into RAM, which the map does not tell
  from data, and a function a few bytes of which were read may be a
  function with a table in it.
- **States the game was never in.** `--entries` starts a function from
  the states a played game entered it in (see [The states](#the-states)),
  which is no help for a function the game did not arrive at while it was
  recorded. Nothing calls such a function for the checker from a moment of
  the game that suits it (a battle for battle code), with arguments
  somebody chose. A state is kept only where control arrives at the first
  instruction of a function the list names, so a function the map has
  inside another gets none. And a function that reads what follows its
  call and returns past it is run from its real state and still thrown
  away, because it does not come back to its return address.
- **The source folder in Visual Studio itself.** The folder
  (see [Working in the source](#working-in-the-source)) configures and
  builds from the command line with the CMake and Ninja that ship with
  Visual Studio's build tools, through the presets Visual Studio reads.
  Nobody has opened it in the IDE: whether it picks the preset up by
  itself, what IntelliSense makes of a file of ten thousand lines, and
  whether F5 starts the game are not known. There is no `launch.vs.json`,
  so a game started from the IDE keeps its save beside the executable in
  `build/`.
- **Merging in the source folder.** A function with comments added and
  nothing else follows an update, comments and all
  (see [Working in the source](#working-in-the-source)). A function whose
  code somebody changed does not: it is kept as it is, whatever the tool
  would now write for it, so a name added to `names.sym` does not reach it,
  and where the export cuts it differently the two are not brought
  together. "Code" is drawn wide: a comment put at the end of a statement's
  line and a name changed inside `GBLIFT_FUNCTION(...)` hold a function back
  as an edited statement does. And a comment about an instruction a
  function no longer has holds the whole function back, where it could have
  been put at a place nearby.
- **What `build` takes on trust.** A function named in `changes.txt` is built
  in whatever the checker says, one it could not run included, and nothing
  looks at whether the change is the one the line describes. An edit built
  from an editor is not checked at all until `build` is run.
- **Other consoles.** Game Boy and Game Boy Color only.
