# macOS

> **From source only, and less tested.** Releases are built for Windows;
> these builds are not tested again for every change, and consoles other
> than the ones named below do not build here yet.

Game Boy, Game Boy Color, Game Boy Advance and Super Nintendo games build as macOS
application bundles (`<id>.app`). The apps are universal, so they run
natively on Apple Silicon and Intel Macs. They need macOS 11 or newer. SDL2 is
inside the app and the ROM is compiled into the executable, so nothing else
has to be installed.

## From another computer: the hub's macOS kit

Without a Mac at hand, use **Compile for export** in the hub (on Windows or
Linux) and tick *macOS (build kit)*. It writes `<id>-macos-kit.tar.gz`: the
recompiled project plus `build.command`. Copy the archive to a Mac, open it,
and double-click `build.command`. It runs the step below and leaves `<id>.app`
next to itself. See [hub/README.md](../hub/README.md#compile-for-export).

A kit copied over USB or a network share opens normally. A kit downloaded
from a browser or received by AirDrop is quarantined, and Gatekeeper refuses
to run its script. Open Terminal, type `bash ` (with a space), drag
`build.command` into the window and press Return. The app it builds was made
on that Mac, so it opens without a warning.

## Build on a Mac

Building needs a Mac: Apple's compiler and SDK only run on macOS. Install the
command line tools, CMake and Ninja once:

```bash
xcode-select --install
brew install cmake ninja
```

Then build a game from the checkout:

```bash
tools/macos/build_macos_app.sh --rom ~/roms/game.gb --name game --title "My Game"
tools/macos/build_macos_app.sh --rom ~/roms/game.gba --name game_gba --trace trace1.txt --trace trace2.txt
```

| Option | Purpose |
| --- | --- |
| `--rom` | ROM file; `.gba` selects the GBA recompiler, `.sfc`/`.smc` the SNES one, anything else the Game Boy one (`tools/consoles.sh`) |
| `--name` | Game id: executable and bundle name (letters, digits, underscores) |
| `--title` | Name shown in Finder and the Dock (default: the id) |
| `--console gb\|gba\|snes` | Override the choice made from the extension |
| `--trace` | `--trace` file for `gbarecomp` or `snesrecomp` (repeatable) |
| `--out` | Destination; defaults to `output/macos` |
| `-- ARGS` | Extra recompiler arguments, such as `-- --compact-resume` |

This recompiles on the Mac, then finishes with the generated project's own
`runtime/cmake/make_macos_app.sh` (`runtime-gba/cmake/` and `runtime-snes/cmake/` for GBA and SNES projects), the
same script a kit's `build.command` runs. The output is `<id>.app` and
`<id>-macos.zip`. The first run downloads the
official SDL2 2.32.10 framework, checks its SHA-256, and caches it in
`~/Library/Caches/native-console`. Builds are cached in `build-macos/`.

The script checks every app before packaging it:

- the executable holds both arm64 and x86_64 code
- it loads SDL2 from inside the bundle
- the code signature verifies

## Saves

Inside an app, battery saves (`<id>.sav`), RTC files and screenshots go to
`~/Library/Application Support/gbrecompiled/<id>/`. The app folder itself is
part of the code signature and is read-only in `/Applications`.
`--save-dir` still overrides this.

## Sharing an app

The app has an ad-hoc signature. That is enough on the Mac that built it. On
another Mac, Gatekeeper blocks a downloaded app that has no Developer ID
signature. There are two ways around that:

- The recipient right-clicks the app, chooses **Open** and confirms. On macOS 15
  and newer, allow it under **System Settings → Privacy & Security** instead.
- Sign with a Developer ID certificate and notarize the app. This needs an
  Apple Developer account.

## Continuous integration

`.github/workflows/macos.yml` covers pull requests that touch the
recompilers, runtimes, hub or tests, but it is opt-in: a pull request runs it
only with the **run-ci** label, and *Actions > macOS and exports > Run
workflow* starts it by hand. macOS runner minutes are billed at ten times the
normal rate, and an unlabelled pull request skips every job, so it costs
nothing. It runs on an Apple Silicon runner and:

1. Builds a generated Game Boy test ROM, two jsmolka GBA test ROMs, gilyon's SNES
   CPU test and a generated SNES battery-save ROM as apps.
2. Runs the apps.
3. Checks that a save lands in Application Support while the bundle's
   signature still verifies.
4. Runs CTest.

The apps are uploaded as a workflow artifact. Commercial ROMs never enter CI.

## Build without a bundle

Without `-DNATIVE_CONSOLE_MACOS_BUNDLE=ON`, a generated project builds a plain
`build/<id>` executable, as on Linux. The hub and the tests use that form. See
[LINUX.md](LINUX.md) for the plain CMake steps.
