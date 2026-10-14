# Native Console Hub

The hub is the desktop front end for Native Console. It keeps a library of
games, turns imported ROMs into native executables with the recompiler
pipeline, and launches the results. It is an SDL2 + Dear ImGui application
built from `hub/` alongside `gbrecomp`.

## What it does

- **Library** with a cover-art grid and a sortable list view. Click a game to
  open its detail panel; double-click or press Play to run it.
- **Import** a single ROM or a whole folder (recursively) from the sidebar, or
  drop files onto the window. Every supported ROM is copied into its own game
  folder, recompiled to C, configured with CMake, and compiled with Ninja on a
  background worker. Progress and logs live in the Jobs view.
- **Jobs** lists every build, cover capture, profile and export, newest
  first, with its log. Finished jobs stay listed, across closing and opening
  the hub, until **Clear history** deletes the list and the logs (the newest
  200 are shown; a game's own `build.log` is not touched). **Cancel** on a
  queued or running job ends it and, on Windows, every process it started:
  the build script, the build tool and the compilers under it. A cancelled
  build leaves the game as it was, the previous build still installed.
- **Metadata**: editable title, free-form description, per-game launch
  arguments, favorites, play count, last-played time, total play time
  (accumulated while a game launched from the hub or its Steam shortcut is
  running, checkpointed every minute), and the installed executable size (logical and on disk).
- **Cover art**: import any PNG/JPEG/BMP/GIF, or capture a frame from the game
  itself. After a successful build the hub automatically captures frame 600 as
  a placeholder cover if none is set.
- **Favorites** page filtered from the same library.
- **Recorded runs** (Game Boy and Game Boy Color): *Record a run* on a built
  game's page. *New run* starts the game from a new save with
  `--record-run`, keeping every button with the cycle it changed on; the
  player's own save is not used. *Continue run* adds a sitting to the newest
  run; *Runs folder* opens `runs/<id>/`, where each run is a folder to send
  whole. What a run is for and how it is replayed: `docs/RUN_RECORDS.md`. A
  backend offers it by returning arguments from `record_run_args`.
- **Texture packs** (Game Boy and Game Boy Color): the detail panel's
  *Texture pack* picker switches between original art and any pack installed
  for that game. *Add pack...* checks that a `.gbtp` file was made for this
  exact ROM and copies it into `games/<id>/texture-packs/`; *Remove pack*
  deletes the selected one; *Packs folder* opens the folder. Play, Steam
  shortcuts, and `--launch` pass the selection as `--texture-pack`. A game
  built before texture packs existed shows a rebuild notice, because older
  executables ignore the argument. See [TEXTURE_PACKS.md](../docs/TEXTURE_PACKS.md).
- **Edited data** (Game Boy and Game Boy Color): a game started with its data
  as somebody has edited it. See [Edited data](#edited-data).
- **Add to Steam**: add one game, your favorites, one console, or the whole
  library to Steam as non-Steam games, with the cover art turned into Steam
  library artwork. See [Steam](#steam).
- **Consoles**: Game Boy and Game Boy Color build today. Game Boy Advance
  builds with `gbarecomp` (see `docs/GBA.md`) and Super Nintendo,
  SA-1 cartridges included, with `snesrecomp` (some titles work, see `docs/SNES.md`),
  NES with `nesrecomp` (some titles work, see `docs/NES.md`), Nintendo DS with
  `ndsrecomp` (not playable yet, see `docs/NDS.md`) and Nintendo 64 with `n64recomp`
  (not playable yet, see `docs/N64.md`). The console list marks each console
  that is not fully playable ("some", "not yet", with the reason when the
  pointer rests on it), and a game's page and Settings say the same
  (`Maturity` in `hub_consoles.h`, which the README's table matches):
  each build first plays the game headless for about 20 seconds (6 for NES,
  30 for Nintendo DS, 50 for Nintendo 64) to find code that static analysis
  misses (trace kept in the game's `profile/` folder). A Nintendo 64 game
  also records, while it is played, the code its build left to the
  interpreter (`<id>.trace` beside the save, kept across rebuilds), so a
  rebuild after some play compiles more of it.
  The ROM is compiled into the executable; saves are `<id>.sav`, SNES and NES
  save states `<id>.state1` to `.state10`. SNES ROMs with coprocessors other
  than the SA-1, and NES ROMs whose cartridge board is not implemented, are
  refused at import with the chip's name or mapper number, and so are
  Nintendo DS dumps whose secure area is still encrypted and DSi-only
  titles.
- **Compile for export**: build ROMs for other computers: a Windows zip, a
  Linux / SteamOS tarball, or a macOS kit that becomes an `.app` on any Mac,
  for the consoles and targets that have been tested.
  See [Compile for export](#compile-for-export).
- **Settings**: every toolchain path is editable and auto-detected
  (one recompiler per console, CMake, Ninja, vcvars64.bat, SDL2), plus build type, compile jobs,
  launch defaults, and cover-capture frame. A release for Windows carries
  SDL2's development files in `SDL2/` beside the hub. What is beside the
  hub is what the hub uses, whatever was saved: its own recompilers, its
  `SDL2.dll` and that `SDL2/` folder, since settings outlive the folder a
  release was unpacked into. Any other saved path is kept while what it
  names exists, and looked for again when it does not. "Wii common key file" names
  the key a retail Wii disc needs to be built (yours to supply; see
  `docs/WII.md`), and "Try Wii games" turns the Wii on, which is not supported
  yet and is off until it is. "GameCube renderer" under
  Launching starts GameCube games with `--renderer software`,
  `--renderer opengl` or `--renderer fast` (the card rasterises and blends:
  much quicker, close but not the same picture; a game built before that
  choice existed has to be rebuilt to take it), or leaves the choice to the
  game's own menu. "GameCube internal resolution" beside it starts them
  with `--internal-scale 1` to `4`, how many times the console's own size
  the fast renderer draws at, or leaves that to the game's menu too (the
  game's menu says under each when it was chosen for the run and what its
  own saved setting is); the
  OpenGL shader cache (`<id>.pipelines`, `<id>.*.glbin`) is kept across
  rebuilds with the saves.

## Running

```bash
./build/bin/native_console_hub --data-dir <dir>
```

Without `--data-dir` (or `NATIVE_CONSOLE_HUB_DATA`) the hub uses
`%LOCALAPPDATA%\NativeConsole\Hub` on Windows and
`$XDG_DATA_HOME/NativeConsole/Hub` elsewhere. The data directory holds:

```text
library.json      game entries and metadata
settings.json     toolchain and preferences
covers/           cover images (<id>.png, or user-imported files)
runs/<id>/        recorded runs of a game, one folder each (docs/RUN_RECORDS.md)
data-trees/<id>/  a game's data as files to edit, made by Make data tree (Edited data)
games/<id>/rom/   the ROM copy
games/<id>/game/  the latest build: <id>.exe, SDL2.dll, and the runtime's save
                  data (<id>.sav, <id>.rtc, <id>.state1..10, imgui.ini)
games/<id>/work/  generation + compile scratch; wiped before each build and
                  deleted after a successful one (kept after a failure for inspection)
games/<id>/edited-data/  the image and pack built from a game's data tree, only for a
                  game that has a tree chosen (Edited data)
games/<id>/       build.cmd|build.sh (re-runnable pipeline), build.log, last-run.log
job-history/      the finished jobs the Jobs view lists: history.jsonl, a line
                  a job, and each job's log (job-*.log). Safe to delete.
hub.log           hub output when running as a windowed app
```

Only the latest build is kept. A rebuild first rescues the save data from the
previous build, wipes the old tree, then installs the new executable next to
that save data. If a rebuild fails, the previous executable stays installed
and playable. Removing a game with "also delete its folder" deletes its save.

Games are recompiled with `gbrecomp --compact-resume` by default (Settings:
"Compact resume points"). Compiled code can then only be re-entered at basic
block starts; a stop mid-block (an interrupt, a frame boundary) resumes through
the interpreter for the rest of that block, which the runtime hands back at the
block's control flow. Guest state is identical, executables are about 12%
smaller, and the only cost is that `--differential` single-stepping of such a
build mostly exercises the interpreter. Turn it off for builds you want to
differential-test.

The **Code coverage** setting picks how much of the ROM the recompiler
compiles: Full (`--pointer-scan loose`), Balanced (`strict`, the default),
Small (`off`), or Minimal (`off --no-scan`). Everything not compiled runs
exactly through the interpreter, which on a modern PC costs about 10% at
worst, so this is a size-versus-speed choice with no correctness impact. On
Link's Awakening the four profiles produce roughly 9.2, 7.1, 6.4, and 1.5 MB
executables before compression.

On Windows the installed executable and DLLs are compressed with NTFS LZX
(`compact /EXE:LZX`) after installation. The OS decompresses transparently on
load, so the game runs directly and nothing else changes; a recompiled Game Boy
executable shrinks roughly 3.8 to 1 on disk. Turn it off in Settings if you
copy games to a non-NTFS drive (the copy is simply uncompressed).

## Edited data

A Game Boy game's data can be taken apart into files, edited, and put
together again into an image the game loads in place of its own ROM
([DATA_FILES.md](../docs/DATA_FILES.md)). On a Game Boy or Game Boy Color game's
page, **Edited data** says where that data is, and every start from then on
uses it:

- **Make data tree** takes the game's ROM apart for you, as a job in the Jobs
  view: the recompiler describes the ROM with the options this hub builds
  games with, `gb_data.py split` writes the files to
  `data-trees/<id>/` in the data directory, and the tree is chosen for the
  game. It takes seconds. A tree that is already there is never written
  over, since its files may have been edited: move or delete the folder to
  start afresh. **Open folder** opens it.
- **Data tree...** chooses a tree made by hand, the folder
  `tools/gb_data.py split` wrote, the one with `manifest.json` in it. Each time the game is started the hub first
  builds the tree (`gb_data.py build`, into `games/<id>/edited-data/`) and
  starts the game with the image, and with the pack beside it when the tree
  has packed pictures (`--rom-image`, `--unpacked`). So the loop is: edit a
  file, press Play. The Play button says so while the tree is built: a
  moment, or some seconds the first time, while its thousands of files are
  read from the disk.
- **Built image...** chooses an image `gb_data.py build` already made. The
  hub passes it as it is, with the `.gbup` pack of the same name if one lies
  beside it. Nothing is built, so this needs no Python.
- The path can be typed or pasted into the field instead. **Clear** goes back
  to the game's own data, and removes what the hub built.

The choice is kept with the game in `library.json` (`edited_data`). A game
with none is started exactly as before, and nothing of the hub's is written
for it.

When a start does not happen, the hub says why and the game is not started
with something else:

- **The tool refuses the tree** (a file of code was edited, a picture or a
  string does not fit): the tool's own message is shown.
- **The game refuses the image or the pack**: it checks both as it starts,
  and exits before it opens a window. The hub reads what it said from
  `last-run.log` and shows it: which byte differs from what the game's code
  was compiled from, or that the pack is of another ROM.
- **The tree is of another ROM**, or the folder or file is gone: said on the
  game's page as soon as it is chosen, and again on Play.
- **The game was built before `--rom-image` existed**: it would ignore the
  argument and start with its own data, so the hub asks for a rebuild.

What a tree calls data has to be data to the built game. *Make data tree*
sees to that by asking the recompiler with the very command a build uses;
it goes by the settings as they are now, so after changing Code coverage
rebuild the game before making its tree. A tree made by hand needs the same
options: `--pointer-scan strict` for the default Code coverage setting,
`loose` for Full, `off` for Small, and `off --no-scan` for Minimal. An edit
that falls where the built game has code is refused by the game, with the
offset.

Making and building a tree take Python and `tools/gb_data.py`. The hub finds Python on
`PATH` and the tool beside itself or in the checkout it was built in; both
can be set under Settings > Toolchain. A release archive carries neither: a
hub from one starts built images, and says so when a tree is chosen.

Play, the link cable's starts, Steam shortcuts and `--launch` all use the
choice. **Record a run** does not: a run is recorded of the game as it is,
and would not replay without the edits.

## Steam

**Add to Steam** is in a game's right-click menu and detail panel, in the
library header, and in the right-click menu of each console in the sidebar.
The header button opens a chooser with a checkbox for every game and one-click
selections for the whole library, your favorites, and each console. It marks
games that are already in Steam; adding them again updates them. Right-clicking
a console opens the chooser with that console's games selected. Only built games
can be added.

The hub finds Steam from the registry and picks the account that signed in
most recently; both can be changed under Settings > Steam. It writes:

- a non-Steam shortcut per game in `userdata/<account>/config/shortcuts.vdf`,
  tagged "Native Console" and the console name. The previous file is kept as
  `shortcuts.vdf.native-console.bak`, and a file the hub cannot parse is never
  overwritten.
- capsule, wide, hero, and icon artwork in `userdata/<account>/config/grid/`,
  built from the cover. Artwork you replace from inside Steam is left alone;
  `steam-artwork.json` in the data directory records what the hub wrote.

Each shortcut runs `native_console_hub --data-dir <dir> --launch <game-id>`.
Generated games are console programs, so launching them directly would open a
console window. The hub starts the game with that window hidden, stays running
until the game exits so Steam shows it as playing, and closes the game if Steam
stops the hub. Play time goes to `play-sessions.jsonl` in the data directory
and is added to the library when a hub window or batch run starts, or within a
few seconds when one is already open.

Steam must be closed while shortcuts change, because it rewrites
`shortcuts.vdf` when it exits. The dialog offers to close it (`steam -shutdown`)
and continues once it has exited. Adding a game again updates its shortcut in
place and keeps its Steam app id, artwork, and Steam play time. **Remove this
hub's games from Steam** in Settings deletes only the shortcuts the hub created.

## Compile for export

Library builds run on the computer the hub is on. **Compile for export**
builds games for other computers. Open it from the sidebar button, or from a
game's right-click menu or detail panel for just that game. Choose ROM files or
a folder of ROMs, tick the targets, and say where it goes — the destination can
be typed or pasted as well as picked, which is how a drive's root is given, and
it is made if it does not exist. Every game becomes one file per target in that
folder:

| Target | File | How it is built |
| --- | --- | --- |
| Windows (x64) | `<id>-windows-x64.zip`: the `.exe`, `SDL2.dll`, a README | MSVC on this computer (Windows hubs only) |
| Linux / SteamOS (x86-64) | `<id>-linux-x86_64.tar.gz`: the executable, a README | Valve's Steam Runtime SDK in Docker (see [LINUX.md](../docs/LINUX.md)); runs on a Steam Deck |
| macOS (build kit) | `<id>-macos-kit.tar.gz`: the generated project, `build.command`, a README | Finished on any Mac (see [MACOS.md](../docs/MACOS.md)) |

A console is exported only for the targets one of its games has been
exported for and run on; the others are refused with a message saying so
(each backend's `export_problem`, through `untested_export_problem` in
`hub_consoles.cpp`):

| Console | Windows | Linux / SteamOS | macOS |
| --- | --- | --- | --- |
| Game Boy, Game Boy Color | yes | yes | yes |
| Game Boy Advance | yes | yes | yes |
| Super Nintendo | yes | yes | yes |
| Nintendo DS, Nintendo 64, GameCube | yes | no | no |
| NES, Wii | no | no | no |

Nintendo DS, Nintendo 64 and GameCube were each exported for Windows with
their first game (the P6 rows of their plans); their Linux and macOS work
(P7) has not started. No NES or Wii game has been exported yet.

The ROM is recompiled once, and every target compiles that same portable C
project; Game Boy Advance ROMs are profiled first, as in library builds. A
game already in the library keeps its id and title. Nothing is
added to the library, and library settings that only fit this computer (NTFS
compression) are not applied. The code-coverage and compact-resume settings
are applied.

Apple's compiler only runs on macOS, so a Mac target cannot be finished on
Windows or Linux. The kit is the part that can be copied over. On the Mac,
double-clicking `build.command` makes a universal `<id>.app` next to it in a
few minutes. The Mac needs the Xcode command line tools, CMake and Ninja, and
the kit's README gives the commands. If Gatekeeper refuses the downloaded
script, run `bash build.command` in Terminal.

Linux exports need Docker Desktop running. The first one
downloads the Steam Runtime SDK image, which is several GB. The Docker path is under
Settings > Toolchain. Exports run as jobs in the Jobs view. Their scratch
folder, `exports/<id>` in the data directory, is deleted after success and
kept after a failure.

## Automation

The same import and build path is scriptable:

```bash
native_console_hub --data-dir <dir> --batch --import path/to/game.gb --import path/to/folder
native_console_hub --data-dir <dir> --batch --rebuild-all
native_console_hub --data-dir <dir> --batch --rebuild <game-id>     # one game; repeatable
native_console_hub --data-dir <dir> --export path/to/roms --export-to D:/exports --export-targets windows,linux,macos
native_console_hub --data-dir <dir> --steam-add all    # or favorites, console:gb, game:<id>
native_console_hub --data-dir <dir> --steam-remove
native_console_hub --data-dir <dir> --batch --launch <game-id>      # start one game as Play does, and wait for it
native_console_hub --data-dir <dir> --batch --data-tree <game-id>   # Make data tree; repeatable
```

`--launch` is what a Steam shortcut runs: no hub window, and a game that
cannot be started is explained in a message box. With `--batch` the
explanation is printed instead and the exit code is 1, so a script can start
a game the way the hub does, edited data included.

`--export` runs without a window, like `--batch`. It is repeatable, and each
value is a ROM or a folder. Without `--export-targets` it uses the targets
chosen last in the dialog.

Batch mode runs without a window, prints every job log line, and exits with 0
only when all jobs succeeded. Each game folder also keeps the generated build
script, which can be re-run by hand.

The Steam commands exit with code 3 without writing anything while Steam is
running. `--steam-root <dir>` and `--steam-user <account-id>` override the
detected Steam folder and account.

## Adding a console

The whole process is in [docs/ADDING-A-CONSOLE.md](../docs/ADDING-A-CONSOLE.md):
it covers the recompiler and runtime, the phases and gates, and platform and
CI wiring. The hub side is:

1. Implement `ConsoleBackend` (`hub_consoles.h`) in `src/console_<family>.cpp`
   (`console_snes.cpp` and `console_nes.cpp` are the examples to copy):
   - ROM identification (`probe`)
   - the recompiler's executable name (`recompiler_name`). Settings finds it
     next to the hub, in `bin/` and on `PATH`, stores it under the family id,
     shows it on the Settings page and checks it: no Settings code to write.
   - the recompiler commands (`generate_steps`, shared by library builds and
     exports; `data_dir` holds anything that must outlive the wiped work
     tree, such as profiles, and `trace_files()` lists the traces in it)
   - the runtime folder inside generated projects (`runtime_folder`, used by
     the macOS kit)
   - optionally, extra CMake arguments (`cmake_args`) and whether the project
     needs SDL2 to configure (`requires_sdl2`). The build script itself is
     shared (`ConsoleBackend::prepare_build`).
   - launch arguments
   - the save files kept across rebuilds (`is_persistence_file`)
   - headless cover capture (`cover_capture_args`), which must not write the
     player's save, and optionally profile runs (`profile_runs`, GameCube:
     3,000 fields of scripted play for its trace), run as a Profile job after
     a build of a game that has a cover and is missing their output. A cover
     capture or Profile job that leaves a `profile/*.trace` the game did not
     have makes the hub rebuild the game with it once it is not running.
2. Register it in `backends()` and add its console ids to `all_consoles()` in
   `hub_consoles.cpp`.
3. Add the source file to `hub/CMakeLists.txt`.
4. Give the console a colour in `console_color()` (`hub_app.cpp`).

Export needs no per-console code. It runs `generate_steps()` once and
compiles the resulting project for each target. That works as long as the
generated project follows the contract in the playbook: it builds
`build/<id>` on its own, embeds the ROM, and depends only on SDL2.

Unsupported backends may return `supported() == false` so their ROMs are still
catalogued. Backends whose executables need files at run time list them in
`install_files()`.

A family does not have to live in this repository. A folder that holds a
`hub_extra.cmake`, at `hub/extra` or wherever `-DNATIVE_CONSOLE_HUB_EXTRA_DIR=`
points when the hub is configured, adds its own sources to the hub and
registers its consoles, backends and export targets through
`src/hub_extra.h`. Steps 2 to 4 above are then done from that folder, and
nothing in the hub's own files names the family.
