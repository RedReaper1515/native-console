# Runtime usage

Each generated project links the recompiled ROM code with the Native Console runtime. This guide covers the generated executable; `gbrecomp` itself has a separate `--help` command.

Examples below use a generated executable named `game`:

```bash
./output/game/build/game
```

## Controls and settings

The default controls depend on the hardware the build targets. Gameplay keys are the same everywhere; shortcuts follow the platform's keyboard. A Mac laptop's F-keys are brightness and volume keys unless fn is held, so Mac builds use Command shortcuts instead.

| Action | Windows / Linux | Mac | Alternate |
| --- | --- | --- | --- |
| D-pad | Arrow keys | Arrow keys | W/A/S/D |
| Game Boy A | Z | Z | J |
| Game Boy B | X | X | K |
| Start | Enter | Return | — |
| Select | Backspace | Delete | Right Shift |
| Fast forward | Tab | Tab | — |
| Toggle maximum speed | Backtick | Backtick | — |
| Quick save | F5 | Cmd+S | — |
| Previous / next state slot | F6 / F7 | Cmd+[ / Cmd+] | — |
| Quick load | F8 | Cmd+L | — |
| Debug overlay | F1 | Cmd+I | — |
| FPS counter | F9 | Cmd+Shift+I | — |
| Native port UI | F2 | Cmd+G | Controller R3 |
| Encounter Lens extension | F3 | F3 | — |
| Mute | M | M | — |
| Settings menu | F10 or Escape | Cmd+, or Escape | — |

A binding may include modifiers (Ctrl, Alt or Option, Shift, Command or the Windows key): hold them while choosing the new key in the settings menu. A binding without modifiers ignores Shift but does not fire while Ctrl, Alt or Command is held, so Cmd+S does not also press S. Preferences store such a key as `key:<scancode>:<modifier bits>`.

Controllers are enabled automatically. The default mapping uses the D-pad or left stick, physical south/east face buttons for Game Boy B/A, and Start/Back for Start/Select. Guide/Home or L3 opens the settings menu; R3 opens the native game panel when the exact-ROM port supplies one. Escape and Android Back also open the settings menu. While a native panel captures input, its D-pad/left-stick and A/B actions do not reach the guest game.

Linux builds are the Steam Deck (SteamOS) build. Steam keeps the Steam and Quick Access buttons, so L3 is the way into the settings menu there; X and Y save and load states, the right trigger fast-forwards and the left trigger toggles maximum speed. Buttons are named as printed on the Deck (A/B/X/Y, L1/R1, L2/R2, L3/R3, View/Menu, and L4/L5/R4/R5 for the back grips when Steam passes them through).

Defaults apply until a control is changed; `runtime_prefs.ini` then holds the full binding list, so a Mac installation that already saved preferences keeps its F-key shortcuts until **Reset Controls**.

The settings menu can remap keyboard and controller gameplay actions and shortcuts. It also exposes audio output, speed, savestate slots, display smoothing, and diagnostic controls. Preferences are stored in SDL's per-application preference directory.

The FPS counter is one line in the window's top right corner, for example `60 FPS  100%`: the emulated frames per second, and the game's speed as a share of the console's own rate, so 100% is full speed. **Show FPS Counter** in the settings menu or its shortcut turns it on, and the choice is kept as `display.fps` in `runtime_prefs.ini`. The debug overlay is separate and is not kept between runs.

The master volume slider runs 0-100 and defaults to 50. The emulated APU mixes at full int16 scale, so the output stage attenuates it: 50 on the slider plays at 2% of raw APU output and 100 at 4%. The value is stored as `audio.master_volume` in `runtime_prefs.ini`; the older `audio.volume_percent` key from the unattenuated scale is ignored.

The Palette selector changes DMG display colors to green, black and white, or amber. It affects the displayed image; diagnostic frame dumps retain the PPU's original colors. CGB games and DMG games running in CGB compatibility mode keep their hardware color palettes, so the selector is disabled in those modes.

## Saves and savestates

Battery-backed RAM and MBC3 RTC data are loaded and saved by the SDL platform layer. Savestates provide ten slots per game and are checked against the ROM and state-format version before loading. The ROM a state is checked against is the one the game was built from: a state made while a ROM image stood in for it (`--rom-image`) loads with another image or with none, and the game says in a line that what the state holds in memory from the other image stays until the game loads the data again (`docs/DATA_FILES.md`).

Treat savestates as local to a compatible runtimes/runtime-gb/game build. A runtime update can intentionally reject an older state when internal hardware state changes. Battery `.sav` files are the more durable persistence format.

## Link cable

Two windows of the same Game Boy or Game Boy Color game can be joined as by
a link cable (trades, battles, two-player modes), on one computer or across
a network. Player 1 hosts and player 2 joins:

- **Hub:** a GB/GBC game's page has *2 Players on this PC*, *Host for other
  PCs* and *Join* with the host's address.
- **In game:** the menu's *Link Cable* section hosts, joins, shows the link
  and disconnects; *Controller* picks which connected controller the window
  reads.
- **Command line:** `--link-host` (`--link-local` accepts this computer only,
  with no firewall question), `--link-join HOST[:PORT]`, `--link-port N`
  (default 47402).

`--player N` (1-4) gives a window its own profile: players 2-4 keep battery
saves, RTC data and savestates in a `Player N` folder beside the game (unless
`--save-dir` names another) and settings in `runtime_prefs_pN.ini`, read the
Nth controller even while the other window has focus, and put the player in
the window title. Player 1 uses the ordinary files. Keyboard input goes to
the focused window only.

How it works (`gbrt_link.c`, `gbrt_link_net.c`): either Game Boy may drive
the clock, so the two run in symmetric lockstep on the host's system-cycle
clock. Each runs at most a lookahead past what the other last reported, and
a transfer takes at least a little longer than that, so the other side
always learns of it in time: every byte, and the moment each side sees it,
matches a real cable. Normal-speed transfers keep their hardware timing on
one computer; CGB fast-clock and double-speed transfers are slowed to that
minimum. A side that is not waiting (SC bit 7 set with the external clock)
when the other drives the clock takes no part and sends $FF. A window that
must wait for the other shows "Waiting for the other player", and savestates
cannot be loaded while the other player is linked. Without a cable nothing
changes: a transfer with the internal clock ends with $FF.

Latency: the lockstep needs to hear from the other side every fraction of a
millisecond of game time. Hosting for this computer only
(`--link-local`, the hub's 2-player button) uses a short lookahead that keeps
full speed up to about 0.5 ms round trip. Hosting for other computers uses a
longer one (transfers take at least 8,192 cycles, about 2 ms): full speed up
to about 3 ms round trip, so wired LAN and good Wi-Fi. Measured with
`GB_NET_DELAY_MS`, a test aid that holds received messages back.

## Execution options

| Option | Purpose |
| --- | --- |
| `--headless` | Run without normal window presentation; useful for tests and automation |
| `--limit <n>` | Stop after `n` guest instructions |
| `--limit-frames <n>` | Stop after `n` completed guest frames |
| `--model auto\|dmg\|cgb` | Select hardware mode; `auto` uses the cartridge header |
| `--input <script>` | Replay inline input events |
| `--record-input <file>` | Record live input as a cycle-anchored script |
| `--control <port>` | Have a program play: the game connects to `127.0.0.1:<port>`, where the program listens, and stops at the start of every frame until it is told which buttons are held and for how many frames. Stopped, it gives the bytes at an address, writes into RAM, and writes or loads a save state. Not with `--benchmark`, `--differential` or `--unpacked-call`. See *A program that plays* below |
| `--record-run <directory>` | Record a played run so that it can be played again to the cycle: each start of the game adds a segment (its buttons by cycle, checkpoints, and the cartridge memory and clock it began with) to the directory, which is made if needed and is where the game keeps its save meanwhile. Loading a save state and the link cable are refused while it records. What `--input` presses is recorded like a player's buttons. Not with `--no-audio` or `--benchmark`. See `docs/RUN_RECORDS.md` |
| `--replay-run <segment.run>` | Play a recorded segment again with nobody at the controls, from the record's own save and clock, writing no save. The exit code is 1 if the replay does not reach the record's checkpoints. Not with `--input`, `--no-audio` or `--benchmark`. A record played with a pack that replaces something (`--unpacked`) is refused without a pack that has the same replacements, and one played with nothing replaced is refused with a pack that replaces something. `tools/run_record.py verify` does it for a whole run |
| `--run-states <directory>` | With `--replay-run`: write a save state every `--run-state-frames <n>` frames (3600 unless given), for `tools/run_record.py sweep` to replay the run in pieces from |
| `--save-dir <directory>` | Place battery RAM, RTC, and savestate files in an existing directory |
| `--data-mod <artifact.gbdm>` | Activate one precompiled, exact-ROM data-overlay artifact without changing the generated executable or embedded ROM |
| `--data-use <file>` | Run in the interpreter and record what the game does with the bytes it reads from its ROM as data: which went to tile data, a tile map, the sprite table, sound or palette registers, the cartridge, or an address jumped to or read through. Written when the game exits; `tools/gb_data.py uses` reads it. It also looks at the screen once a frame and notes which layer drew each of those tiles and through which palette, which `gb_data.py tiles` draws a picture in, and how the bytes in its tile maps lay and which tiles they drew, which `gb_data.py maps` makes grids of tile numbers of. See `docs/DATA_FILES.md` |
| `--unpacked <file>` | Load a pack of unpacked data, the `.gbup` that `tools/gb_data.py build` writes beside the image: it names the game's own unpacking routines, and holds the edited pictures to have in place of what they make. Each is put there as its routine returns, and only where what the routine made is, by its hash, what the picture was drawn from. A pack of another ROM stops the game. A run recorded while a pack replaces something has the replacements' count and hash in its record, and replays only with them (`docs/RUN_RECORDS.md`). See `docs/DATA_FILES.md` |
| `--unpacked-harvest <file>` | With `--unpacked`: keep what each of the pack's routines makes, once for each different thing made of each packed stream, and write it out when the game exits; `tools/gb_data.py unpacked` reads it. The game plays the same |
| `--report-unpacked` | Print how often each of the pack's routines was entered, returned, had what it made replaced, and made something else of a stream that has a replacement |
| `--unpacked-call <bank:address[,name=value...]>` | With `--unpacked`: once the game has started and any `--load-state-file` is loaded, enter that function of the game (hexadecimal) in place of going on, and stop as the first routine of the pack returns, when what it made is kept for `--unpacked-harvest`. After the address, each behind a comma and in hexadecimal, what to set first, in order: a register (`a=0D`, `de=4000`), a byte at an address (`D0AA=01`: RAM, or below 8000 a write to the cartridge, which maps a bank), `ime=1` or `ime=0`. The exit code is 1, and a line says why, if the function comes back with no routine having returned, or none has after `--unpacked-call-cycles`. Not with `--benchmark`, `--differential`, or a recorded run. `tools/gb_data.py unpack` uses it to have the game unpack streams nobody played as far as. See `docs/DATA_FILES.md` |
| `--unpacked-call-cycles <n>` | With `--unpacked-call`: how many cycles to wait for a routine to return (41943040 unless given). Cycles and not frames: a game with its screen off never ends a frame |
| `--data-use-follow <bank:address>` | With `--data-use`: follow the calls in progress while the instruction at that address reads the ROM (hexadecimal, as `gb_data.py uses` lists the instructions that read), and write into the record which functions they were, how each was entered, what it read and what it stored to RAM. Repeatable, up to four. `tools/gb_data.py unpackers` reads it to find the routine that unpacks. See `docs/DATA_FILES.md` |
| `--data-use-pointers` | With `--data-use`: write into the record as well which two bytes of the ROM made each address the game read its ROM through (two bytes it read as data, or an instruction's operand), what was read through each pair, and which bytes the game compared or reckoned with (for an operand, also whether the game jumped to what it made). `tools/gb_data.py pointers` reads it to say what can be moved into free space, which is how a string made longer is built. The record is otherwise the same, and the game plays the same. See `docs/DATA_FILES.md` |
| `--data-use-tiles` | With `--data-use`: also record where in a tile each byte landed that reached the tile area of video memory as it is, so that a picture can be made of such bytes without being told where the tiles start or how many bits a pixel they are. The record gets a `tile_runs` section, and is otherwise what it is without the option; `tools/gb_data.py uses` and `tiles` read it. See `docs/DATA_FILES.md` |
| `--trace-back <file>` | Run in the interpreter and record, for every write to the bytes of RAM named with `--trace-back-of`, which instruction wrote, inside which calls, and what the value it wrote was made of: bytes of the ROM read as data, constants in instructions, hardware registers that were read, and earlier writes to RAM, each of those followed back the same way. Written when the game exits; `tools/gb_trace_back.py` prints it as a tree. The game plays the same. See *A backward trace from a RAM address* below, and `docs/DATA_FILES.md` |
| `--trace-back-of <address[-address]>` | With `--trace-back`, and needed by it: a byte of RAM to watch, or a range of them, in hexadecimal from `8000` up. Repeatable, up to sixteen |
| `--trace-back-depth <n>`, `--trace-back-nodes <n>` | With `--trace-back`: how far back a tree is followed, in sources one behind another (256 unless given) and in events for one tree (4096). What is cut off is counted in the record |
| `--trace-back-trees <n>`, `--trace-back-each <n>` | With `--trace-back`: how many trees the record holds (64 unless given), and how many of them one writing instruction gets with one set of calls around it (1). Every write is counted, with or without a tree |
| `--trace-back-memory <megabytes>` | With `--trace-back`: how much memory the trace remembers events in (128 unless given; a `k` after the number makes it kilobytes, 16 at the least). When it is full the oldest events are written over, and a tree that leads back to one says that it is forgotten |
| `--rom-image <file>` | Use a ROM image in place of the ROM built in: the one `tools/gb_data.py build` makes from the game's data files, edited or not. It has to be the same size, and the same in every byte of code and of the cartridge header, or the game says which byte and does not start. See `docs/DATA_FILES.md` |
| `--host-configuration <file>` | Load a canonical applied host configuration for a native-patch package that declares an exact configuration contract |
| `--rtc-unix-time <seconds>` | Use a fixed Unix timestamp for deterministic MBC3 RTC load/save and replay |
| `--ignore-rtc-persistence` | Start RTC registers clean while still loading battery RAM; intended for explicitly isolated test routes |
| `--no-audio` | Disable APU/audio emulation for CPU/PPU-focused runs. The sound registers then read `$FF`, which a game can tell from a Game Boy: a run cannot be recorded or replayed with it, and a state dump taken with it is not to be compared with one taken without |
| `--smooth-lcd-transitions` | Re-present completed frames during long guest frames; enabled by default |
| `--no-smooth-lcd-transitions` | Disable the host-side long-frame smoother |
| `--native-presentation native\|original` | Select host or original presentation for native-patch packages; packages default to `native`, unpatched projects to `original` |
| `--port-ui-open` | Start a compiled native port module with its UI shown |
| `--disable-port-module` | Diagnostic failure injection: skip attachment of a compiled port module so exact-ROM native patches can prove they fail closed when their host surface is unavailable |
| `--port-toggle-frame <n>` | Deliver a native-UI toggle after guest frame `n`; repeat in strictly increasing order for multiple toggles |
| `--port-input-frame <n>:<action>` | Deliver `toggle`, `encounters`, `close`, `open-pc`, directional, `accept`, or `back` to the port UI after guest frame `n`; repeat in increasing order |
| `--port-state <file>` | Write port lifecycle/render counters, ordered source-built extensions, and the final renderer-independent command frame as JSON |

`--model cgb` runs a DMG cartridge in CGB compatibility mode. `--model dmg` rejects a CGB-only cartridge.

`--host-configuration` is emitted only when an exact-ROM native-patch manifest
declares a host-configuration contract. The runtime requires canonical JSON,
the declared schema/version and policy identity, and values within the declared
bounds before it creates a guest context. Missing configuration means the
feature is disabled. Malformed, non-canonical, incompatible, or out-of-range
files exit before guest execution. Diagnostics and `--dump-state` expose only
the policy and SHA-256 identity, never the configuration path. Configuration
is host-owned and does not alter battery RAM or the cartridge save schema.
When a compiled native panel applies a new configuration, the runtime validates
the same exact contract, writes canonical JSON through an atomic replacement,
and only then publishes the new live value. Challenge Mode changes take effect
at the next battle boundary, never in the middle of an active battle.

Data overlays are opt-in. Without `--data-mod`, every ROM read uses the
untouched user-provided ROM. With it, the runtime validates the artifact ABI,
exact ROM size and SHA-256, ordered non-overlapping ranges, bounds, and every
expected source byte before guest execution. Any failure exits without
activating a partial or previously loaded overlay. CPU, OAM DMA, and HDMA
cartridge reads then resolve through the immutable overlay; cartridge
identity, persistence compatibility, and the owned ROM allocation continue
to use the original bytes. Live semantic ROM readers see the active overlay.
Use `gbrt_semantic_reader_init_live_original` when a tool explicitly needs the
exact original ROM view.

For modded replay, do not treat `--input` and `--data-mod` as sufficient
provenance. Use the portable replay envelope documented in
[Data-mod packages](DATA_MODS.md). Its driver validates ROM, executable,
generated source inventory, ordered packages and content, schema contract,
artifact, configuration, and embedded input hashes before launching this
executable.

`--input` and `--record-input` are for short scripted repros. A run somebody
plays, of any length and over several sittings, is recorded with
`--record-run`: it keeps the start as well as the buttons, and checks itself
when it is replayed. The two go together where a scripted run has to be had
again exactly: `--record-run` with `--input` records what the script presses.

### A program that plays

A script (`--input`) presses what somebody wrote down before the game
started. A program that plays has to look first: where the player stands,
whose turn it is, what the cursor is on. `--control <port>` is for that. The
program listens on a port of the machine, starts the game with the option,
and the game connects and stops at the start of every frame. While it is
stopped the program asks what it wants to know, and then says which buttons
are held and for how many frames the game is to run before it stops again.

`tools/gb_control.py` is the program's end, for one written in Python:

```python
from gb_control import Game

with Game(["output/game/build/game", "--headless", "--save-dir", scratch],
          watch=[(0xC000, 0x2000)]) as game:       # work RAM comes with every stop
    while game.byte(0xD35E) != 40:                 # until a byte of it says so
        game.step("A")                             # a frame with A held
        game.step()                                # and one with nothing held
    game.save_state(scratch / "there.state")
```

`step(buttons, frames)` takes letters as `--input` has them (`U D L R A B`,
`S` for Start, `T` for Select) and returns false when the game closed
instead of stopping: at its `--limit-frames`, with its window, at the end of
a replay. `read(address, length)` asks the game, `byte()` and `bytes_at()`
answer from what is watched, `write()` puts bytes into RAM, `save_state()`
and `load_state()` take a path, and `close()` closes the game as a player
would. Run as a command it plays some frames and prints memory:

```bash
python3 tools/gb_control.py output/game/build/game --frames 600 --hold S \
  --read FF80:10 -- --headless --save-dir output/scratch
```

A read is of what a byte holds and changes nothing in the machine. No
register is brought up to date for it, so the video and sound registers at
`FF10`-`FF4B` are as the runtime last left them, which may be some cycles
ago; the memory a game keeps its own state in is exact. Cartridge memory
reads whether or not the game has it switched on, and the ROM reads as it is
mapped.

What the program holds is buttons like a player's, on top of whatever a
player or a script holds. So a game played by a program can be recorded
(`--record-run`): every press is in the record on the cycle it reached the
game, and the run replays with no program there. While a run is recorded or
replayed the game refuses to have a byte written or a state loaded, since
neither would be in the record; it still writes a state when asked. A replay
can be watched frame by frame through `--control`: the game then sees the
record's buttons and not the program's.

With a window the game runs at the speed a person plays it, and with
`--headless` as fast as the program answers. If the program goes away the
game closes with exit code 1. What goes over the connection is written down
in `runtimes/runtime-gb/include/gbrt_control.h`, for a program in another language.

### A backward trace from a RAM address

`--data-use` follows the bytes of the ROM forward, to the screen and the
speaker. What that cannot find is what only steers a game: a formula, the
tables it looks numbers up in, the constants in its instructions. Those are
found from the other end. Name a variable, and the game says of each write to
it where the value came from:

```bash
# From a save state made shortly before the thing of interest happens.
./output/game/build/game --headless --save-dir output/scratch \
    --load-state-file output/before.state --limit-frames 600 \
    --trace-back output/hp.json --trace-back-of CFE6-CFE7

python3 tools/gb_trace_back.py show output/hp.json --rom roms/game.gb \
    --sym output/src/game/names.sym --export output/game-export.json
```

The game runs in the interpreter while it is traced, as it does with
`--data-use`, and lifted source is left out for the run. Every store to RAM
becomes an event: the instruction, the address, the value, and up to eight
sources the value was made of. A source is a byte of the ROM read as data, a
constant in an instruction, a hardware register that was read, an earlier
event, or RAM as it was when the trace began; and it took part as a value, as
an address the value was loaded through, or as a decision, which is something
a conditional jump, call or return tested for the code that then ran. The
first time each instruction writes a watched byte, with each set of calls
around it, the record gets the events that write leads back to. `docs/DATA_FILES.md`
says how to read the tree the tool prints, and what the trace cannot see.

It only reads. A traced game ends in the state it ends in untraced, a
`--data-use` or `--evidence` record made beside it is byte for byte what it
is alone, and it goes with `--load-state-file`, with `--control` (a state
the program loads starts the trace's knowledge again; a byte the program
writes shows as changed from outside), and over a replayed run
(`--replay-run`), which still reaches every checkpoint. It is refused with
`--differential`.

What it costs, measured on one game (Pokémon Red, 18,000 frames replayed
headless): 1,990 frames a second traced, against 2,230 with `--data-use` and
3,470 recompiled with nothing recorded; 153 MB of memory, 128 of them the
events. That game makes about 535 events a frame, 37 bytes each, so 128 MB
remember a little under two minutes of play: start from a save state close
to what is to be traced, or give it more with `--trace-back-memory`.

Input entries can be frame anchored, such as `120:S:1`, or cycle anchored,
such as `c4412912:S:8192`. Periodic cycle pulses use
`p<start>-<last-start>/<period>:<buttons>:<duration>`; for example,
`p1000000-2000000/50000:A:10000` presses A for 10,000 cycles every 50,000
cycles. The duration must be shorter than the period. Periodic entries retain
the complete duration of the final pulse and avoid expanding long automation
routes into thousands of parser entries. New recordings use individual cycle
anchors because they preserve the exact observed press duration.

`--save-dir` is intended for isolated replay and test sessions. It fails if the
directory does not already exist, and keeps `.sav`, `.rtc`, and savestate files
out of the user's normal preference directory.

Battery and RTC loads require the exact expected file size and stage data
before changing guest state. A malformed file is rejected visibly, and
automatic persistence writes are suppressed for that process so teardown
cannot overwrite the rejected input. Writes use a `.tmp-v1` transaction,
flush and synchronize its complete bytes, then atomically replace the primary
file where the host supports it. Short writes, failed synchronization, and
failed replacement retain the previous `.sav` or `.rtc`; a stale stage is
ignored by loading and safely truncated by the next successful transaction.
Battery RAM remains a raw cartridge-compatible image. RTC serialization v2
uses a fixed 40-byte, little-endian layout and accepts v1 files for migration.
`--rtc-unix-time` controls the wall-clock elapsed-time component;
guest-executed RTC cycles still advance normally.
`--ignore-rtc-persistence` is not normal play behavior and must be disclosed by
any evidence that uses it.

Exact-ROM source integrations can layer validated state edits on that atomic
persistence boundary with `gbrt_semantic_transaction_begin`, bounded
`gbrt_semantic_transaction_write` calls, a staged-reader validator, and
`commit` or `abort`. Mutable ranges are limited to external RAM and fixed or
banked WRAM. A commit persists the complete staged battery snapshot before
publishing staged bytes to live guest memory; persistence failure therefore
rolls back without exposing a partial edit. The latest transaction sequence,
outcome, and merged dirty ranges appear under `semantic_transaction` in
`--dump-state` JSON for deterministic replay evidence. The lifecycle is a
synchronous safepoint operation: guest execution must remain paused from
`begin` through `commit` or `abort`.

Port ABI v3 exposes that lifecycle to reviewed source-built modules as the
one-shot `run_semantic_edit` service. The runtime begins the transaction,
invokes a callback that may stage and validate bounded semantic records, then
commits only if the callback returns success with a still-active validated
transaction. The callback-scoped transaction must not be retained, committed,
or aborted by the module. Failure is aborted before guest execution resumes.
The same ABI exposes path-free host-configuration identity, a runtime-owned
atomic Apply callback, and explicit modal input capture. It does not grant a
module direct filesystem access.

The three `--port-*` options are present only when the generated project
compiled an exact-ROM port module. They work in `--headless` mode without a
graphics device. `--port-state` reports host/module lifecycle state plus the
ordered active extension identities and final panel/text command frame. Port
state schema v3 adds `input_captured` to the v2 extension inventory; it remains separate from
`--dump-state`, which is guest-only. See
[Port and frontend modules](PORT_MODULES.md) for the ABI and activation rules.
The separate [native presentation contract](NATIVE_PRESENTATION.md) defines
completed-frame map, tile, sprite, UI, timing, and accurate-PPU shadow data
for future exact-game renderers.

## Capture and logging options

| Option | Purpose |
| --- | --- |
| `--log-file <file>` | Redirect runtime output to a log |
| `--trace` | Enable verbose runtime tracing |
| `--trace-entries <file>` | Record executed bank/address entry points, a line a dispatch. `gb_lift.py coverage` counts such a file by what ran: lifted C, recompiled code inside a lifted function, or neither (see `docs/LIFTING.md`) |
| `--evidence <file>` | Record every jump the game makes that its code does not spell out (a `JP HL`, a `RET` to an address no `CALL` pushed): the instruction, where it went, and the `CALL` that entered the function it happened in. Written as JSON when the game exits, closing its window included (a killed game writes nothing), for `gb_lift.py edges --evidence` (see `docs/LIFTING.md`). An unpaced run loses about 1% of its speed, and the game ends in the same state. A game with lifted source runs its recompiled bodies while recording |
| `--executed <file>` | Run in the interpreter and note every byte of the ROM that was part of an instruction the game executed, the first byte of an instruction apart from the later ones. Written when the game exits; `gb_lift.py executed` sorts an export's functions by it and by what `--data-use` saw read as data: ran, read and never executed, or neither (see `docs/LIFTING.md`, *What the game ran*). The game ends in the same state as without |
| `--entry-states <file>` | Run in the interpreter and keep the registers and the whole of memory as they were when control arrived at the first instruction of a function: the first time with each return address, and the first, second, fourth, eighth... arrival. Written when the game exits, for `gb_lift.py check --entries`, which runs lifted C against the ROM from those states (see `docs/LIFTING.md`, *The states*). The game ends in the same state as without. A game with lifted source runs its recompiled bodies while it keeps them |
| `--entry-states-of <file>` | With `--entry-states`: the functions, one `BANK:ADDRESS` in hexadecimal a line (`#` starts a comment), as `gb_lift.py entries EXPORT -o FILE` writes them from an export. Without it the functions are the places a `CALL`, a `RST` or an interrupt arrived at |
| `--entry-states-per <n>` | With `--entry-states`: keep at most this many states of one function (default 8) |
| `--dump-state <file>` | Write the final machine state as JSON |
| `--save-state-file <file>` | Write a complete binary savestate when execution stops |
| `--load-state-file <file>` | Resume normal execution from a compatible complete savestate; intended for local diagnostics and replay calibration. A state goes with any `--rom-image` of its game; with `--replay-run`, only with the image the run is of |
| `--dump-frames <list>` | Dump selected guest frames |
| `--dump-present-frames <list>` | Dump every host present associated with selected guest frames |
| `--screenshot-prefix <path>` | Set the output prefix for frame captures |

Keep reproducible artifacts under `logs/`:

```bash
./output/game/build/game \
  --log-file logs/game.log \
  --record-input logs/game.input

./output/game/build/game \
  --log-file logs/game-replay.log \
  --input "$(cat logs/game.input)"
```

## Interpreter fallback diagnostics

Writable WRAM/HRAM code executes from its current bytes through runtime helpers and interpreter fallback. It is not statically cached: changing an opcode or operand, including self-modification during a call, takes effect on the next fetch. ROM control flow can also target the middle of another instruction; each reachable entry is decoded separately. The synthetic `generated_dynamic_code_end_to_end` test covers both paths of `jr z` over `db $3E; xor a`, repeated RAM edits, and guest-written instruction operands. This is not a whole-game compatibility guarantee.

Generated execution also falls back to the interpreter when the analyzer did not emit a compiled target. Instrument the fallback before assuming it is responsible for a slowdown:

```bash
./output/game/build/game \
  --log-file logs/interpreter.log \
  --limit 500000 \
  --log-frame-fallbacks \
  --report-interpreter-hotspots \
  --interpreter-hotspot-limit 12

python3 tools/summarize_interpreter_log.py logs/interpreter.log
```

| Option | Purpose |
| --- | --- |
| `--log-frame-fallbacks` | Report the first and last fallback site in affected frames |
| `--report-interpreter-hotspots` | Print the complete cause-aware fallback-site inventory plus aggregate interpreter hotspots at shutdown |
| `--no-lift` | Only in a game generated with `gbrecomp --lift` (see `docs/LIFTING.md`): run every subroutine's recompiled body, as if nothing were lifted |
| `--report-lift` | Only in such a game: print at shutdown how many lifted functions ran, how often lifted code ran guest code, and how many frames ended inside it |
| `--lift-signatures FILE` | Only in such a game: hold every lifted function to its signature (`docs/LIFTING.md`, *Signatures*). Each time one returns to its caller, the registers and flags it keeps are put back as they came and the ones it leaves undefined are given a value that is nobody's. For testing signatures against the original; a run cannot be recorded or replayed with it |
| `--lift-callers FILE` | Only in such a game: note where each lifted function returned to while the game was played, and write it when the game stops (`docs/LIFTING.md`, *Who calls what, from the game played*). Changes nothing in the game |
| `--interpreter-hotspot-limit <n>` | Limit the aggregate hotspot table |

The fallback inventory is independent of the hotspot limit. Each
`[INTERP] Fallback site` row reports the runtime bank/address, handoff count,
interpreted instruction and cycle totals, first/last frame, reason, and the
number of compiled bank variants at that address. Reasons are:

- `bank_not_compiled`: the address is dispatchable in other banks, but not the
  active bank;
- `address_not_compiled`: the generated page exists, but the address does not;
- `page_not_compiled`: no generated dispatch page exists for the address;
- `writable_hram`: immutable compiled dispatch is unsafe for writable HRAM.

The leading inventory row reports `complete=yes` only when every distinct site
fit in the runtime's diagnostic inventory. Treat `complete=no` or a nonzero
`dropped` count as a failed measurement. Interpreter control-flow exits
(including jumps, calls, returns, restarts, HALT, and STOP) are included in the
per-site instruction and cycle totals.

If the summary says `No interpreter fallback recorded`, investigate runtimes/runtime-gb/device work instead of analyzer coverage.

## Differential execution

Differential mode advances generated execution and the interpreter from matching initial states and stops at the first detected divergence:

```bash
./output/game/build/game \
  --differential 500000 \
  --differential-log 100000 \
  --differential-fail-on-fallback
```

| Option | Purpose |
| --- | --- |
| `--differential [steps]` | Compare for a bounded number of steps; default is 10,000 |
| `--differential-frames <n>` | Stop after `n` completed frames |
| `--differential-state <file>` | Load the same complete savestate into both comparison contexts |
| `--differential-log <n>` | Print progress every `n` matched steps |
| `--differential-no-memory` | Skip full mutable-memory comparisons for a faster, weaker check |
| `--differential-log-fallbacks` | Log generated-to-interpreter fallback during comparison |
| `--differential-fail-on-fallback` | Treat any fallback as a failure |
| `--differential-inject-mismatch <step>` | Perturb interpreter state at one step to self-test mismatch detection |

This is a recompiler-vs-interpreter consistency check, not an independent hardware oracle: both paths share mapper, PPU, DMA, timer, and audio implementations.

## Performance diagnostics

`--debug-performance` enables the main slowdown signals. For a stable scene, combine it with input recording:

```bash
./output/game/build/game \
  --log-file logs/performance.log \
  --record-input logs/performance.input \
  --debug-performance
```

Individual options include:

- `--log-slow-frames <ms>`
- `--log-slow-vsync <ms>`
- `--log-lcd-transitions`
- `--debug-audio`
- `--debug-audio-seconds <n>`
- `--debug-audio-trace`
- `--audio-stats`

### Generated execution counters

Generated projects can compile diagnostic transition and device-attribution counters for optimization work. They are off by default and do not participate in savestates, state dumps, or differential comparisons.

```bash
cmake -G Ninja -S output/game -B output/game/build-counters \
  -DCMAKE_BUILD_TYPE=Release \
  -DGBRECOMP_ENABLE_PERFORMANCE_COUNTERS=ON
ninja -C output/game/build-counters

./output/game/build-counters/game \
  --headless \
  --limit-frames 1800 \
  --report-performance-counters
```

The final `[PERF-COUNTERS]` record reports whether instrumentation was compiled in; tick commits/cycles; generated safepoints and transitions; generic and specialized generated reads/writes; interpreter fallback; PPU calls/dots/draw work/rendered pixels/stable spans; audio samples; timer, audio, RTC, DMA, and serial activity; interrupt/frame stops; and visibility-region estimates. `[PERF-HISTOGRAM]` records report tick size, PPU mode by calls and cycles, timer state, next-event deadline, visibility boundaries, and estimated region sizes. A normal build accepts the reporting option but prints `available=0` and zero base counters.

Memory counters describe generated memory operations, not every runtime or device access. A specialized operation used a generated WRAM/banked-WRAM/HRAM path or an explicitly enabled benchmark-only I/O path; a generic operation retained `gb_read8`/`gb_write8` and all of its mapper, bus, DMA, MMIO, and model behavior. Stable-span dots are a subset of rendered PPU pixels, while draw dots also include scalar startup, stall, sprite, window-edge, and final-transition work.

The conservative visibility-region estimator is deliberately separate from basic attribution because it is more expensive:

```bash
./output/game/build-counters/game \
  --headless \
  --limit-frames 1800 \
  --report-performance-counters \
  --estimate-visibility-regions
```

It groups only register-only and already-proven safe-memory work, stops at generic memory, transitions, fallback, stopped state, or the current exact deadline, and estimates removable tick/PPU/safepoint commits without changing execution. Use it for one diagnostic capture, not timing trials.

### Timer A/B control

Generated projects use arithmetic DIV/TIMA advancement by default. `--scalar-timer` selects the retained one-T-cycle timer oracle for correctness checks and same-binary performance comparisons; it is diagnostic, not a recommended player setting. The control changes no savestate or generated-code format.

```bash
python3 tools/compare_nl0_controls.py \
  --before output/game/build/game \
  --after output/game/build/game \
  --before-arg=--scalar-timer \
  --input-file tools/profiles/game.input \
  --frames 9000 \
  --repeat 8 \
  --warmup 1 \
  --json-out logs/timer-ab/artifact.json
```

Compare counters only across matching ROMs, inputs, frame limits, models, feature flags, and executable hashes. Their purpose is to explain a timing change, not replace state hashes or independent accuracy tests. Keep counters off for release/runtime measurements.

### Reproducible full-headless attribution

`tools/run_nl0_profile.py` generates one fresh project, builds symbolized counters-off and counters-on Release variants, measures cold/warm build and runtime wall/RSS, runs the estimator separately, checks identical repeated state hashes, samples named leaf coverage on macOS, and writes one provenance-rich JSON artifact. The input must contain only cycle-anchored `c<cycle>:<buttons>:<duration>` entries.

```bash
python3 tools/run_nl0_profile.py \
  --name game-profile \
  --rom path/to/game.gb \
  --gbrecomp build/bin/gbrecomp \
  --project-dir output/game-profile \
  --build-root output/game-profile-build \
  --input-file tools/profiles/game.input \
  --frames 9000 \
  --repeat 4 \
  --warmup 1 \
  --sample-seconds 5 \
  --json-out logs/game-profile/artifact.json
```

The default diagnostic build uses generated `-O3`, debug symbols, frame pointers, and IPO off so stacks remain attributable. `--ipo` creates a separately labeled LTO profile. Summarize raw counter logs with `tools/summarize_nl0_profile.py`; use `tools/compare_nl0_controls.py` for an interleaved profiling-off regression gate against a retained binary snapshot.

Generated Ninja projects use 1 MiB code chunks and cap executable-target compilation at eight concurrent jobs to bound compiler memory without serializing runtime/UI compilation. Override the limit at configure time with `-DGBRECOMP_GENERATED_COMPILE_JOBS=<n>`, or set it to `0` to disable the pool. `tools/profile_generated_build.py` produces comparable cold/warm build and footprint artifacts; the retained measurements are in `docs/history/NL4_GENERATED_BUILD_RESULT_2026-07-14.md`.

### Audio capture and diagnostics

The APU advances channel events, DIV-driven frame-sequencer edges, and sample deadlines chronologically. Its exact rational sample clock emits 44,100 stereo frames per 4,194,304 Game Boy system cycles, independent of scheduler batch size. This includes HALT fast-forward and CGB double-speed execution.

Normal execution accumulates sub-sample APU time and publishes it at the next exact sample deadline or before an APU/PCM MMIO observer, DIV reset, speed switch, savestate, or diagnostic report. Use `--eager-audio` only for same-binary regression measurements against the former per-tick schedule. The retained gate and artifacts are documented in `docs/history/APU_EVENT_BATCHING_RESULT_2026-07-14.md`.

`--debug-audio` writes `debug_audio.raw` in the current directory. The file contains interleaved stereo signed 16-bit samples at 44.1 kHz in host byte order. Limit the capture with `--debug-audio-seconds <n>`. The analyzer currently expects that filename in its own working directory, so keep the capture in an artifact directory and run:

```bash
mkdir -p logs/audio-capture
(cd logs/audio-capture && ../../output/game/build/game --headless \
  --limit-frames 720 --debug-audio --debug-audio-seconds 10)
(cd logs/audio-capture && python3 ../../tools/analyze_audio.py)
```

Use `--debug-audio-trace` to write APU register and activity diagnostics to `debug_audio_trace.log`. `--audio-stats` reports queue depth, underruns, and related output metrics. These captures describe the emulated APU stream before host-device resampling or mixing, so they are suitable for deterministic PCM hashes.

## Benchmarking

Use the benchmark helper instead of timing a normal interactive window:

```bash
python3 tools/benchmark_emulators.py path/to/game.gb \
  --recompiled-binary output/game/build/game \
  --frames 1800 \
  --repeat 5 \
  --warmup 1 \
  --json-out logs/game-benchmark.json
```

The helper creates a dedicated optimized generated build by default and runs the executable with `--benchmark`.

`--benchmark` is a reduced-workload, core-oriented profile. It disables pacing and host presentation, audio emulation, final RGB conversion, and pixel rasterization while retaining CPU/device timing work. Results are useful for comparing the same profile across revisions; they are not full interactive-runtime measurements. Use `--no-recompiled-autobuild` only when you intentionally want to measure the exact binary already on disk.

## Test-only options

The accuracy runner uses these options for deterministic test-ROM execution:

- `--stop-on-test-breakpoint` stops when a Mooneye-style `LD B,B` sentinel executes.
- `--dump-state <file>` captures registers, machine state, full HRAM, fixed
  WRAM bank 0 (`C000-CFFF`), switchable WRAM bank 1 (`D000-DFFF`), the first
  256 bytes of available external RAM (`eram_a000_a0ff`), and dispatch
  fallback count for pass/fail inspection. `total_cycles` is the monotonic
  64-bit replay clock; unlike the hardware-facing 32-bit `cycles` field it
  remains ordered after a 32-bit wrap. The explicit WRAM banks make
  symbol-backed game-state checkpoints possible without depending on the
  active `SVBK` value at process exit.

Normal games should not use `--stop-on-test-breakpoint`.
