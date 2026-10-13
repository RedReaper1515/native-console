# Native Console

@AGENTS.md

AGENTS.md, imported above, is the working manual. This file adds what it
leaves out: the owner's standing rules, parts of the tree it does not
describe, and what a cloud session can and cannot check.

## Standing rules

- **Windows first.** A console has to be working on Windows (correct
  picture, real audio, full speed, playable) before any Linux, SteamOS or
  macOS work or testing for it, which is the playbook's P7. Do not start
  or schedule P7 work for a console unless asked. A passing gate is not
  the same as working: dump frames and audio and check them before calling
  something done.
- **One pull request per change.** Branch from `origin/main`, push to
  `origin`, and open a PR against `main` in the repository `origin` names.
  Say in the description what was tested and on which platform. Merge it
  on GitHub only when asked to in the same conversation. Before a parent
  PR's branch is deleted, retarget any PR stacked on it to `main`.
- **Do not spend Actions minutes.** No job runs for a branch push or for a
  PR without the `run-ci` label (such a PR may show a run with every job
  skipped), so leave PRs unlabelled. Do not add `run-ci`, start or re-run
  a workflow, or push a `v*` tag (a tag publishes a release) unless asked.
  A PR whose checks are skipped or absent has not been built by anything.
- **No fixes for individual games in a runtime.** When a game shows
  something wrong, the runtime's implementation of the console is wrong:
  fix it there, with a test (`docs/ADDING-A-CONSOLE.md`, P8). Mods and
  ports of one game, never a workaround for a runtime bug, go in the
  opt-in, exact-ROM packages: `NATIVE_PATCHES.md`, `PORT_MODULES.md`,
  `DATA_MODS.md`.
- **The goal is moddable games.** A native executable alone is what a
  bundled emulator would give. Weigh work by what it gives modders:
  function overrides, pre and post hooks, symbol maps, lifting functions
  by hand, data and texture packs. Rank it above marginal accuracy tuning.
- **No user ROMs, discs, BIOS files or saves in git or in a workflow.**
  `.gitignore` does not cover every extension (BIOS and firmware dumps,
  save states and arcade disk images have no pattern), so read
  `git status` before staging.
- **Scripted runs must not touch a player's files.** A generated game
  keeps its save beside its executable, and a Game Boy game rewrites it on
  exit, headless too. Pass `--save-dir <existing scratch directory>` to
  every scripted run, `--prefs <scratch file>` to the GBA, SNES, NES,
  Nintendo DS and GameCube frontends (the DS keeps its settings and the
  console's firmware memory beside that file), and
  `--data-dir <scratch directory>` to the hub.
- **`gh` and remotes.** Push only to `origin`, and pass `--repo` with the
  repository `origin` names (`git remote get-url origin`) to every `gh`
  command. Where an
  `upstream` remote exists, `gh` targets `arcanite24/gb-recompiled`, and a
  PR there publishes this work in someone else's repository.

## What AGENTS.md leaves out

- **GameCube** is in the root build: `recompiler-gc/` (`gcrecomp`),
  `runtime-gc/`, `tests/gc/`, `tools/gc/`. Its only document is
  `docs/GC-PLAN.md`, a dated log of about 5,500 lines; there is no
  `GC.md`. The plan's Status table and Layout block are out of date, so
  trust the tree and the later-dated sections, and find a topic with
  `grep -n '^## ' docs/GC-PLAN.md`. The hardware notes are
  `docs/gc-*.md`, one per subsystem.
- **Wii** is the GameCube family's second console, not a family of its
  own: the same `gcrecomp` and `runtime-gc/`, with the Wii-only code in
  files named `wii_*`, tests in `tests/wii/` and tools in `tools/wii/`.
  `WII.md` and `docs/WII-PLAN.md` are its documents. A change to shared
  code has to leave the GameCube exactly as it was: run the `gc_*` tests as
  well as the `wii_*` ones. No Nintendo key goes into the repository, a
  test or a log; the tests build their own disc around made-up keys. The
  hub's Wii backend stays `supported() == false` until a retail game has
  passed P6 ("Try Wii games" in the hub's Settings, or `NATIVE_CONSOLE_WII=1`,
  turns it on for whoever is testing).
- **Families from outside.** The hub can be built with console families
  whose code is not part of the public repository (`hub/src/hub_extra.h`).
  Where a checkout has a `hub/extra/` folder, read its `README.md` before
  touching the hub: it says which folders are private, and nothing outside
  those folders may name what is in them — no source file, document, test
  or workflow.
- **Modding documents** missing from the AGENTS.md map: `DATA_MODS.md`,
  `DISCOVERY.md`, `TEXTURE_PACKS.md`, `VOXEL_VIEW.md`,
  `NATIVE_PRESENTATION.md`.
- **Game Boy references.** `tech_docs/pan_docs.md` and `SameBoy/` are
  gitignored, like `roms/`, and nothing in the repo fetches them. Where
  they are absent, read Pan Docs (gbdev.io/pandocs) and the SameBoy source
  (github.com/LIJI32/SameBoy) online. If neither can be reached, stop and
  say so; do not change timing from memory.

## Cloud sessions

This section applies only to a cloud session (`CLAUDE_CODE_REMOTE=true`).
On a machine with MSVC and `roms/`, use the AGENTS.md commands and exclude
nothing.

A cloud session is a fresh clone on Ubuntu 24.04 with about 4 cores. It
has GCC, Clang, CMake, Ninja and Python, and nothing that is not in git:
no commercial games or discs, no `roms/`, no devkitPPC, no Dolphin, no
MSVC, and no graphics card to rely on. Memory notes from the owner's
machine do not exist there.

### Build and test

```bash
apt-get update && apt-get install -y ninja-build libsdl2-dev   # once; better in the environment's setup script
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
ctest --test-dir build --output-on-failure -j4 -E accuracy_runner_exit_policy
```

Run on 2026-10-06 at `6c83bc1` in an `ubuntu:24.04` container with 4
CPUs and GCC 13.3: the build took about 3 minutes, CTest about 100
seconds, and 120 of 122 tests passed.

- **Two tests already fail on Linux**, both GameCube: `gc_fpu_fast` (the
  fast path's status flags differ from the reference under GCC) and
  `release_bundle_is_self_contained` (its generated GameCube project does
  not compile under GCC). Neither is yours to fix unless that is the task.
- Run the build and the tests in the background: they outlast the default
  two-minute command limit.
- Name the directory `build`: several tools default to `build/bin`.
  `test_gameplay_mutation` is built only if tests were on at the first
  configure; later, add `-DGBRECOMP_ENABLE_NATIVE_PATCHES=ON`.
- Without `libsdl2-dev` the configure still succeeds, but the Game Boy
  runtime, the hub, every `*_play` frontend and about forty tests are
  dropped, and generated Game Boy projects fail to configure.
- `accuracy_runner_exit_policy` and `tools/run_tests.py` need Mooneye and
  Blargg ROMs under `roms/` that have no fetch script. Exclude the test,
  as the macOS workflow does; the focused runs are the owner's.
- AGENTS.md's fresh-project check needs no game: the
  `generated_*_end_to_end` tests in the run above build and run fixture
  projects. By hand, a legal Game Boy ROM is
  `python3 tests/fixtures/make_mapper_rom.py --mapper mbc1 --output output/fx.gb`.
- To run a windowed frontend by hand, set `SDL_VIDEODRIVER=dummy` and
  `SDL_AUDIODRIVER=dummy`. The hub's `--batch` and `--export` open no
  window.

### What a green run does not show

- Tests that need fetched suites are not registered when `roms/<console>/`
  is absent. Fetch one suite at a time, for example
  `python3 tools/fetch_gba_tests.py --suite jsmolka`,
  `tools/fetch_snes_tests.py --suite gilyon` or
  `tools/fetch_nes_tests.py --suite accuracycoin`, then configure again.
  The ROM baselines need more: `gba_test_roms_baseline` wants `jsmolka`
  and `alyosha`; `snes_test_roms_baseline` wants `gilyon`, `peterlemon`,
  `higan` and `absindx`. Without `--suite` a script downloads every
  suite, 1 to 3 GB. Fetched data stays in `roms/` and is never committed.
- The Nintendo DS test-ROM tests (`nds_test_roms_baseline`,
  `nds_recomp_*`, `nds_wrestlers`, `nds_polyrastertest_rom`) are not
  registered after a fetch alone: their ROMs are built in the BlocksDS
  Docker image (`tools/nds/build_test_roms.py`) and their reference data
  is written with DeSmuME (`tools/nds/oracle_make_*.py`). The DS unit
  tests, the menu self-test and `nds_recompile_fixture`, which uses a ROM
  the repository makes itself, need none of it: 18 tests matching `nds_`
  passed in the Steam Runtime SDK container (GCC 10.3) at `5beb6a2`.
- `gc_state_roundtrip`, `gc_state_roundtrip_fresh_process`,
  `gc_card_restart` and `gc_menu_selftest` pass while checking nothing
  when the devkitPPC-built guest programs are absent, and
  `gc_hwtests_baseline` is not registered at all.
- `tools/gc/run_checks.py` needs those programs, and a game disc
  (`--disc`) for its game stages. It is a gate for the owner's machine,
  as are the OpenGL path, real audio and speed measurements.

### Reporting

- State what ran on Linux and what is left for the owner to check on
  Windows: the MSVC build, real games, picture, sound, speed.
- `output/` and `logs/` are not in git and end with the session: put the
  evidence (commands, test counts, hashes) in the PR description.
- A Linux build and test run here checks shared code; it is not P7. Fix
  what your change breaks. NES, Nintendo DS and GameCube have not reached
  P7: if their
  tests already fail on an untouched `main`, report it in the PR and do
  not chase it unless that is the task.
