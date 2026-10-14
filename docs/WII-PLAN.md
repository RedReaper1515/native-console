# Wii backend: plan

Started 2026-10-06 on branch `feature/wii-backend`.

The Wii is the GameCube's design with more of everything, and this backend
is built that way: it is a second console of the GameCube family, not a
family of its own. It follows [ADDING-A-CONSOLE.md](ADDING-A-CONSOLE.md);
this document records what is specific to the Wii, the gates, and their
results. [GC-PLAN.md](GC-PLAN.md) has everything the two consoles share.

## Ground rules

- **One runtime, two consoles.** Broadway is Gekko at 729 MHz with four more
  BAT pairs; Hollywood contains Flipper unchanged. The processor core, the
  recompiler, the command processor, both renderers, the video interface,
  the controller ports, the sound path, save states, the frontend and the
  overlay menu are `runtimes/runtime-gc/` and `recompilers/recompiler-gc/` as they are. A Wii
  adds files named `wii_*` to those directories and a mode the machine is
  created in. The hub shows Wii as its own console (id `wii`) served by the
  `gc` family backend, the way `gbc` is served by the Game Boy family.
- **Nothing a GameCube game can see changes.** A Wii's state hangs off a
  pointer that already existed (`GcCpu::machine`), so `GcMachine`'s layout
  is untouched and save states made before the Wii existed still load. The
  GameCube tests (20 in CTest, the hwtests baseline among them) are run
  before and after every change to shared code.
- **No firmware and no keys.** IOS is emulated at the level a game sees it:
  the mailbox registers and the request blocks, with C code behind them. No
  part of IOS, the system menu or boot2 is used. No Nintendo key is in this
  repository: the common key that unlocks a retail disc comes from the user
  (see [Keys](#keys)).
- **License policy** as in the playbook. Dolphin and the four existing Wii
  recompilers are GPL: behaviour references only, never copied.
- **One semantics source, explicit guest state, interpreter from day one,
  labelled evidence layers, fail closed** — inherited from the GameCube
  backend unchanged.
- **Windows first.** No Linux or macOS work for the Wii until a game plays
  on Windows (the owner's standing rule; P7 is not started).

## Contract deviations

The GameCube's, unchanged: the disc's content ships beside the executable
as one `ID.gcdata` file (a hard link to the library's copy where the volume
allows). One addition: the generated executable carries the **title key of
the disc it was built from**, so the installed game needs no key file. That
key is specific to the user's own disc and is useless for any other.

## Keys

A retail Wii disc's game partition is encrypted with a per-title key, which
is itself stored on the disc encrypted with Nintendo's common key.

- The repository, the release archives and the tests contain no Nintendo
  key. The tests build a synthetic disc with keys made up for the purpose.
- The user supplies the common key once: `WII_COMMON_KEY` (32 hex digits) or
  `WII_KEY_FILE` in the environment, or `wii-common-key.bin` / `keys.bin`
  beside the image (a raw 16-byte file, 32 hex digits of text, or a BootMii
  `keys.bin`). A key is accepted only if it decrypts the disc's first
  cluster to data whose hash checks; a wrong key is reported as wrong.
- `gcrecomp` derives the title key at build time and embeds it, so nothing
  the hub installs or exports needs the common key again.
- Most other projects embed the common key in source (the survey below
  records which). That precedent was noted and not followed: the cost of
  asking the user for sixteen bytes once is small.
- RVZ and WIA images store the title key already decrypted and the data
  unencrypted, so supporting them (P8) removes the need for a common key
  for users whose library is in that format.

## What we reuse

Checked 2026-10-06 with `gh api repos/OWNER/REPO/license`; where that gave
no answer the license file was read.

| Source | License | What we take |
|---|---|---|
| `runtimes/runtime-gc/`, `recompilers/recompiler-gc/` (this repository) | MIT | Everything a Wii shares with a GameCube |
| [devkitPro/libogc](https://github.com/devkitPro/libogc) v3.1.0 (`c70bdf2`) | zlib (its `wiiuse/` is GPL-3: not read for code; `lwbt/` is BSD) | The client side of every IOS request: numbers, structure layouts, vector formats. What the test programs are built with. |
| [doldecomp/ogws](https://github.com/doldecomp/ogws) `src/revolution/BTE` | Apache-2.0 (Broadcom's stack) | What a retail game's Bluetooth stack sends and expects, read as a reference. The SDK decompilation beside it (IPC, NAND, WPAD) is reference only. |
| [xwiimote](https://github.com/xwiimote/xwiimote) `doc/PROTOCOL` | MIT-style | The remote's report protocol, extensions, EEPROM layout |
| WiiBrew wiki | no content license declared | Facts only (register addresses, request numbers), re-expressed; nothing pasted |
| [encounter/nod](https://github.com/encounter/nod) | MIT OR Apache-2.0 | Reference for WIA/RVZ (P8). Rust; not linked. |
| Dolphin `docs/WiaAndRvz.md` | GPL-2.0-or-later repository | The RVZ format's specification: implemented from, not pasted |
| zstd, liblzma, bzip2 | BSD-3 / 0BSD / BSD-style | RVZ/WIA decompression (P8), not yet vendored |
| achurch.org `ppc750cl.s` | "No copyright is claimed" | Per-instruction 750CL test, register verdict (P1) |
| [dolphin-emu/hwtests](https://github.com/dolphin-emu/hwtests) `iostest/` | GPL-2.0 | Built and run as a tool, as the GameCube backend already does with `cputest` and `gxtest` |
| [noumidev/nouwii](https://github.com/noumidev/nouwii), [mkwcat/saoirse](https://github.com/mkwcat/saoirse) | MIT | Device-side structure of ES/FS/DI, read for shape; nothing copied so far |
| [encounter/decomp-toolkit](https://github.com/encounter/decomp-toolkit) | MIT OR Apache-2.0 | The `symbols.txt` format, for naming recompiled functions (modding) |
| Petari (Super Mario Galaxy), mscharged, zeldaret/tp, ogws, zeldaret/ss | CC0-1.0 | Symbol maps as facts, for hooks |
| Dolphin `Source/Core/Core/IOS`, `HW/WiimoteEmu`, `HW/DSPHLE/UCodes/AXWii.cpp` and `Zelda.cpp` | GPL-2.0-or-later | Behaviour oracle only: `docs/gc-dsp-zelda.md` is written from the last and the code from that page. Facts taken as data: each IOS's version, date and MEM2 layout (`IOS/VersionInfo.cpp`, read out of the IOS binaries by that project) and the sound microcodes' identifications. Dolphin itself is run as a separate program, for reference frames and for a second machine's memory to compare with. |
| wiiparty-recomp, Wiicompiled, DolRecomp/ModernGekko, WiiRecomp | GPL-2.0 / GPL-3.0 | Not used. The only existing Wii recompilers; all date from 2026 and none is copyable. |
| wiiuse, cwiid, fakemote, wit, libwbfs, mini, StarStruck | GPL | Reference or tool only |
| eigenform/ironic, zaydlang/beanwii, rnconrad/WiimoteEmulator, devkitPro/wii-examples | no license | Tool only, never vendored |

## Layout

```
runtimes/runtime-gc/include/wii.h          the Wii's state, Hollywood, Broadway, boot state
runtimes/runtime-gc/include/wii_ios.h      IOS: request blocks, the device interface
runtimes/runtime-gc/include/wii_crypto.h   AES-128 and SHA-1 (from the standards)
runtimes/runtime-gc/src/wii.c              console selection, Hollywood's registers, interrupts
runtimes/runtime-gc/src/wii_ios.c          the kernel: descriptors, acknowledgements, answers
runtimes/runtime-gc/src/wii_ios_es.c       /dev/es
runtimes/runtime-gc/src/wii_ios_di.c       /dev/di
runtimes/runtime-gc/src/wii_ios_fs.c       /dev/fs and the flash filesystem's files
runtimes/runtime-gc/src/wii_nand.c         the flash filesystem's contents, one file on the host
runtimes/runtime-gc/src/wii_sysconf.c      SYSCONF and setting.txt, as a new console's
runtimes/runtime-gc/include/wii_bt.h       the Bluetooth controller's state and its interface
runtimes/runtime-gc/include/wii_remote.h   a remote's input as the frontend sets it
runtimes/runtime-gc/src/wii_ios_bt.c       /dev/usb/oh1/57e/305: the device a game opens
runtimes/runtime-gc/src/wii_bt.c           the controller: HCI, L2CAP, service discovery
runtimes/runtime-gc/src/wii_remote.c       the remotes: reports, memory, camera, extensions
runtimes/runtime-gc/src/wii_disc.c         partitions, title keys, WBFS
tests/wii/                        unit tests and tests/wii/programs (libogc guests)
tools/wii/                        build_wii_programs.py and friends
```

## Phases

| Phase | What it is for the Wii | Gate |
|---|---|---|
| P0 | This plan, the license survey, libogc as reference, the test-program builder | The survey is recorded; `tools/wii/build_wii_programs.py` builds; reference data fetch runs clean twice |
| P1 | Broadway: PVR, HID4, BAT4-7, 729 MHz timebase | The Gekko vectors still pass; achurch `ppc750cl.s` reports no failures; `wii_hello` identifies a Broadway |
| P2 | MEM2, Hollywood registers and interrupts, the IOS kernel, ES, STM, DI, the flash filesystem; encrypted discs and WBFS | `wii_*` guest programs pass interpreted; hwtests `iostest` matches its baseline; the synthetic-disc tests pass; a retail disc boots through its apploader to its first frame |
| P3 | Video: nothing new in GX. Progressive and EuRGB60 modes; XFB and textures in MEM2 | gxtest unchanged on a Wii machine; frames match Dolphin on a Wii homebrew scene and on the game |
| P4 | `gcrecomp` on Wii discs and Wii ELFs; code in MEM2 (REL modules) interpreted, then traced | Every `wii_*` program gives the same verdict recompiled; `--differential` clean |
| P5 | Remotes over the emulated Bluetooth controller (buttons, accelerometer, pointer, Nunchuk, Classic), AX for Wii, saves in `ID.nand`, frontend mapping and menu | `wii_padtest` passes against both libogc's stack and a retail game's; 30 s without underruns; the menu self-test passes with a remote mapped; a save survives a rebuild |
| P6 | Hub: Wii discs probe as console `wii`, key handling in Settings, cover, persistence | A game the user owns imports, builds, plays, saves; `--differential` clean over 6,000 fields; native share reported |
| P7 | Platforms and export | Not started (Windows first) |
| P8 | RVZ/WIA, speed, MotionPlus, speaker, save states for Wii, symbol maps for hooks | Per item |

## Checklist coverage

| Nuance | Test |
|---|---|
| MEM2 at 0x90000000 / 0xd0000000, distinct from MEM1; block ends | `wii_hello` test 2 |
| Bus 243 MHz, core 729 MHz, 27 cycles a video clock; a field is still 1/59.94 s | `wii_hello` tests 1 and 4 |
| Broadway PVR; BAT4-7 behind HID4[SBE] | `wii_hello` test 1; (bus unit test to add) |
| IOS open/close/ioctlv, descriptor reuse, unknown device is -6 | `wii_hello` test 3 |
| Request acknowledged before answered; one outstanding flag at a time | `wii_hello` (libogc's client depends on both); hwtests `ipc_timing` to add |
| Hollywood registers answer at 0x0d000000 and 0x0d800000 | to add |
| DI/SI/EXI/AI at 0x0d006000 as well as 0x0c006000 | `wii_hello` (libogc uses the 0x0d addresses) |
| Encrypted partition, title key, hash check, cluster cache | `wii_disc_*` |
| WBFS and CISO containers | `wii_disc_*` |
| GameCube unaffected by any of it | the 20 `gc_*` tests |

## Status

| Phase | State |
|---|---|
| P0 | **Met 2026-10-07.** Plan and license survey written; `tools/fetch_wii_tests.py` fetches libogc v3.1.0, the Apache-licensed BTE stack, the achurch instruction test and wii-examples at pinned commits or hashes, and a second run does nothing; `tools/wii/build_wii_programs.py` builds 16 programs. |
| P1 | PVR, HID4, BAT4-7 and the 729 MHz clocks are in and tested (`wii_machine`, `wii_hello`). The achurch `ppc750cl.s` test runs as `wii_cputest`: **268 instruction results differ from a 750CL's** (see [What the instruction test found](#what-the-instruction-test-found)), so the gate as written - no failures - is not met; the debt is the processor core's, shared with the GameCube, and is recorded in the baseline. dolphin-emu/hwtests `cputest`, built for the Wii, gives the GameCube baseline's numbers exactly. |
| P2 | MEM2, Hollywood, interrupts, the IOS kernel, ES, STM, DI and the flash filesystem (ISFS, SYSCONF, setting.txt, one host file) are in; encrypted discs read from ISO, CISO and WBFS; a disc built here boots through its partition, an apploader of ours and a DOL (`wii_boot_disc`). The Bluetooth controller is in (`wii_bt`). **No retail disc has run.** One was supplied on 2026-10-07 (SB4E01, a WBFS in 2 MB sectors): its container, partition table, ticket and TMD read, and it stops at the lock, as it should, because no common key has been given yet. hwtests `iostest` measures IOS's timings rather than judging them and does not compile against a current libogc; it is not run. |
| P3 | The five GameCube drawing programs, built for the Wii, draw the same frame byte for byte on both machines (`wii_frames_match_gamecube`), and hwtests `gxtest` built for the Wii gives the GameCube baseline's numbers (`wii_hwtests_baseline`). Progressive and EuRGB60 modes not looked at. |
| P4 | `gcrecomp` takes Wii ELFs (`--wii`) and Wii discs; the generated program selects its console and carries its disc's title key. The boot disc recompiled is 99.9 % native, gives the interpreted verdicts with no key file present, and is differential-clean. The program set recompiled: see the results below. |
| P5 | The frontend selects the console from the disc, keeps `ID.nand` beside the card, and maps the remote four ways (remote + Nunchuk, sideways, Classic Controller, GameCube controller) with the mouse as the pointer. The Bluetooth controller and up to four remotes (buttons, accelerometer, pointer, Nunchuk, Classic Controller) reach a program through libogc's own stack: `wii_padtest` passes 5 of 5 with a Nunchuk and with a Classic Controller, interpreted and recompiled, and `wii_bt` plays a game's stack against the controller (693 checks). Not done: the extension cipher with a key other than zero, which a retail game's library sets (logged when it happens, and the extension's data reads wrong); MotionPlus; the speaker; pairing by the sync button; a retail game's stack, which only a retail game can show. Sound for Wii games (no ARAM; a different AX command set in most) is not written: a first step, sharing the GameCube voice code, is parked unmerged. |
| P6 | The hub has a `wii` console served by `console_gc.cpp`, a Wii key file setting, and builds the synthetic disc in 7.9 s. Unsupported until a retail game passes; "Try Wii games" in Settings, or `NATIVE_CONSOLE_WII=1`, turns it on. |
| P7 | Not started (Windows first). |
| P8 | Save states (2026-10-08, above). The rest not started. |

### Test inventory (2026-10-07)

| Test | What it holds |
|---|---|
| `wii_crypto` | AES-128 and SHA-1 against FIPS-197, SP 800-38A and RFC 3174 |
| `wii_disc` | The disc reader on a synthetic disc: three containers, every way of supplying a key, wrong keys refused, the formats turned away |
| `wii_make_test_disc` | The disc builder's command line |
| `wii_machine` | 62 checks: memory map, BAT4-7 behind HID4, Hollywood registers and mirror, pins, timer, the mailbox handshake and its interrupt |
| `wii_nand` | 885 checks: every filesystem operation and error, limits, the host file's round trip and a damaged one refused, the default console, SYSCONF and setting.txt read back independently |
| `wii_fs_device` | 703 checks: `/dev/fs` and files through the mailbox, each request's buffer layout, the SDK's save and settings sequences replayed |
| `wii_programs_baseline` | 15 guest programs on the Wii machine: `wii_hello`, `wii_fstest`, `wii_padtest`, `wii_cputest` (268, a debt) and the eleven GameCube programs built for the Wii, all passing |
| `wii_bt` | 693 checks, the test playing a game's Bluetooth stack: start-up as libogc's stack and as Broadcom's do it, a remote's connection and channels, every report, the camera, both extensions, service discovery, flow control, four remotes at once, and the same run twice giving the same bytes at the same cycles |
| `wii_padtest_nunchuk`, `wii_padtest_classic` | libogc's own stack and remote library read a remote the runner holds a known way, once with a Nunchuk and once with a Classic Controller: 5 of 5 each |
| `wii_hwtests_baseline` | dolphin-emu/hwtests `cputest` and `gxtest`, built for the Wii, against the GameCube's baseline: identical |
| `wii_frames_match_gamecube` | Five drawing programs: same frame on both consoles |
| `wii_boot_disc` | A disc boots from WBFS, ISO and CISO; recompiled from the disc it runs with only its embedded key and agrees with the interpreter |

The `gc_*` tests pass unchanged, and Shrek 2 built from main and from this
branch ends 1,800 scripted fields at the same state hash in the same time.

## What the first retail game found

Super Mario Galaxy 2 (SB4E01, asks for IOS56), 2026-10-07. With the owner's
common key the disc opened, its own apploader ran and the game started -
and stopped, time after time, each time on something the programs written here
could not have shown. In the order it met them (the count grew past six):

1. **The sound microcode is not AX.** The game uploads Nintendo's own
   ("Zelda", `d643001f`) and waits for the second word of its greeting. The
   DSP now tells the families apart by the bytes uploaded and speaks this
   one's protocol (`gc_zelda.c`, `docs/gc-dsp-zelda.md`, `gc_zelda`). Its
   voices came later ("The sound", below). GameCube games that use the
   family (The Wind Waker, Super Mario Sunshine, Pikmin) were stopped by
   the same thing and have not been tried.
2. **A display list recorded at run time was executed while it was being
   recorded.** The SDK's GXBeginDisplayList points the processor's FIFO at
   a buffer and unlinks it from the graphics processor's; the two were one
   FIFO here. They are two now (`gc_gx.h`, `GC_GX_UNLINKED`; `gc_gx`'s
   "a display list recorded through an unlinked FIFO"). Shared code:
   a GameCube game that records lists was wrong the same way.
3. **Addresses in MEM2 that were never turned into offsets**: an EFB copy's
   destination, the displayed buffer, a texture's image, a palette's
   source, the vertex arrays' bases and the frame the runner dumps. The
   game keeps its three display buffers and most of its textures in MEM2,
   so every frame went nowhere and every texture read as nothing.
4. **The remote.** The game's own Bluetooth stack (Broadcom's) brought the
   module up and connected the remote with no change. It sets the
   extension's cipher key, which is not implemented, as expected.
5. **Out of memory while loading.** What IOS leaves in low memory depends
   on the IOS: from IOS30 a game's share of MEM2 ends at 0x935E0000, not
   0x933E0000. The game sizes its permanent heap from that and fills it to
   within two megabytes. Found by running the game under Dolphin as a
   separate program, reading its memory through its GDB stub, and putting
   the two machines' heaps side by side (26.09 MB against 24.09 MB). The
   table of each IOS's figures is in `wii.c`.
6. **The register write mask.** Blitting-processor register 0xFE masks the
   one write after it. Nintendo's engine sets a material's blend factors
   that way, leaving the colour- and alpha-update bits alone; heard as
   whole writes they turned both off, and nearly everything drawn in three
   dimensions wrote depth and nothing else. Shared code (`gc_gx`'s "the
   register write mask"). Shrek 2 uses the mask too, always with the whole
   register's value behind it, which is why it never showed.
7. **The depth texture.** The game clears its depth buffer by drawing a
   rectangle whose depth is a texture's colour (registers 0xF4 and 0xF5).
   Neither renderer had it, so the rectangle wrote the nearest depth there
   is and the whole scene failed the depth test behind it. In the reference
   rasteriser and in both OpenGL programs now (`gc_gx`'s "the depth
   texture"); on this game the exact OpenGL path's frames are the reference's
   pixel for pixel at the three places compared.

It reaches the title screen ("Press A and B") and, pressed, the file select
with its three planets, recompiled, at about a console's speed on the
reference rasteriser (2,400 fields in 36 s).

**Choosing a file, and the Nunchuk's cipher.** Choosing a file made the
scene vanish in one frame, forty-odd fields after the press, with thousands
of NaNs in its objects, where under Dolphin the chosen planet comes forward
and the game asks whether to create a new file. The cause was the Nunchuk:
the game's library sets the extension's cipher key, the cipher was not
implemented, and what the game read as the Nunchuk's calibration, stick and
accelerometer was noise. With the cipher in (`wii_extension_cipher.c`, the
owner's decision to take Hector Martin's devcrypto under its MIT option;
`THIRD_PARTY.md`) the game asks its question, as it does under Dolphin. The
key the game's library wrote is a test vector now (`wii_bt`).

**Into the game.** With the pointer on "OK" (an `--input` script's second
stick is where the remote points) the game writes its banner and
`GameData.bin` to the flash, asks for an icon, shows the new file ("World
1: The Great Space Journey Begins") and, started, tells its story: the
storybook pages, Peach's letter, and then the prologue's first scene, in
play - Mario out of the pipe in front of the castle, star bits falling,
"The Star Festival had just begun." That is 6,000 fields of a script that
presses A; nobody has played it by hand.

A wrong turning on the way is worth recording, because it was written up
as success for an hour. The first script pressed A with the pointer beside
"OK", not on it. Nineteen seconds later the screen closed in a
Bowser-shaped wipe and opened on Mario standing on Starship Mario, and
that was taken for the game going into play. Nothing had been written to
the flash (`WII_IOS_TRACE` shows it), which is what gave it away. Why the
game does that from that press is not known; it still does with everything
below fixed, and it does not in the windowed frontend with the same script,
whose machine differs from the headless runner's from the second field on
for a reason that has not been looked for either.

**The floating-point unit switched off.** With the real path found, the 3D
scene behind the file screen vanished at field 3,718 of every run,
whatever was pressed: a blue screen with the 2D layout still on it. The
camera's matrix was NaN; so, a field later, was everything else.
`--trace-invalid` (new) put the first NaN in the game's
matrix-to-quaternion routine, on a branch that is impossible for any
matrix: the routine's compares had been given numbers that were not its
own. The processor never took the floating-point unavailable exception,
so the game's threads all computed in one set of floating-point registers
(`docs/GC-PLAN.md`, "The floating-point unit switched off", has the
mechanism and the fix, which is the whole family's). With it the scene
stays, and the script above runs.

**The sound.** The microcode's mixer is `gc_zelda_mix.c`, written from
`docs/gc-dsp-zelda.md`: a voice's parameter block, its sample sources (the
block-coded format, plain samples, the wave generators), resampling, two
filters, mixing by destination with volume ramps or by position into four
corners, and four reverb delay lines. Left at its title screen, what the
game gives the audio interface is the same to the bit as what it gives
Dolphin's, for all 1,277,378 stereo samples (39.9 s) Dolphin was run for
(`tools/wii/compare_audio.py`; the document's "Evidence" says what that
stretch of music exercised and what it did not). `gc_zelda_mix` holds the
rest to the document with voices worked out on paper. A voice now reports
that it has ended, which the silent version never did. Through the opening
and into the prologue (6,000 fields) the game also sends voices to
destinations rather than placing them - 63,526 voice-frames of 260,663 -
and plays sixteen-bit samples as well as block-coded ones; the title music
does neither, so those paths have run under a game and have not been
compared with anything.

What was ruled out before that was known, each with evidence: the game waiting on IOS
(no request is outstanding; `WII_IOS_TRACE`), the Mii database missing from
the flash (the face library carries on without it, as its decompilation
says it will), a NaN the game makes itself at the title screen (Dolphin's
memory has the same NaN in the same object), and the processor's compare
and select instructions on a NaN (read, and right).

Tools this left behind: the runner's end-of-run lines (what is displayed
and where the last frame went, graphics totals, IOS requests and disc
reads, the sound microcode), `--dump-memory FILE`, `--watch ADDRESS` and
`--trace-invalid FIELD` (who wrote a value, and where a NaN was born),
`WII_IOS_TRACE=1` (a game's IOS requests with the field each was made in),
the windowed frontend's `--play-input` pointing the remote where a script
says, and
`tools/wii/oracle_dolphin_game.py`, which runs the same disc under Dolphin
with scripted presses of A and B and brings back its memory, screenshots
and sound, and `tools/wii/compare_audio.py`, which lines two dumps of a
game's sound up by content and compares them sample for sample.

## What the first hands-on play found (2026-10-08)

The owner played Super Mario Galaxy 2's prologue for twelve minutes on the
fast renderer and reported four things. What each turned out to be:

**"The tulips are darker than they should be, and flash."** A light with
no direction. The game lights its flowers with two lights whose cone is
off (`a0 = 1`, `a1 = a2 = 0`) and never gives them a direction; the three
words hold whatever its stack held, and from the moment the first Toad runs
past that is not a number. The spot attenuation multiplied it by nought and
got not-a-number, the channel came out black, and the flowers were left
with their material's constant term, a third as bright. The cosine is now
clamped to zero before the quadratic sees it, and a cosine that is not a
number is zero (`attenuate` in `gc_gx_draw.c`; `docs/gc-lighting-texgen.md`
A.4.2). That is Dolphin's reading, not a measurement. It is the transform
unit's, so every renderer had it; `gc_gx`'s "a spot light's direction"
holds it. Found with a probe that printed the state of every triangle
covering one pixel on a bright field and a dark one - the registers were
the same and the lit colour was black in both, which is what pointed at the
light.

**"The reflectiveness on the star seems to be off."** (2026-10-09.) The
baby Luma looked like a cracked eggshell lying in the grass and whenever it
turned, and smooth only facing the camera at rest. A coordinate generator
was being fed the position after its position matrix and the normal after
its normal matrix, where the hardware feeds it the vertex's own: the Luma
is skinned, each vertex names a texture matrix for its bone, and each bone
was turning its normals twice. The software renderer drew the same cracks,
which is what said it was the transform unit and not the fast path.
`texgen_source` in `gc_gx_draw.c` now reads the vertex
(`docs/gc-lighting-texgen.md`, B.1, which had this down as its most
consequential open question); `gc_gx`'s "a generator's source" holds it.
Every game's position- and normal-sourced coordinates change with it -
reflections, projected textures - wherever the position matrix is not the
identity; Shrek 2's first 3,700 fields have none and are the same frames.

**"Lag spikes, and the lag makes the sound stutter."** Not spikes: while
Mario is running and jumping the machine's own thread needed 17 to 18 ms of
a 16.7 ms field, the game ran at nine tenths of its speed, and the sound ran
dry (34 times in 100 s at the console's size, 56 at twice it). The game
drew every field; nothing was waiting. Where the time went and what came
out is in `docs/GC-PLAN.md`, "A range's first and last page": 15.5 ms to
13.3 ms a field on the same scene, the machine's state identical, and no
dropout in 25 s of it at the console's size (one at twice it). That is not
room: a heavier scene will not hold, and the next thing in the profile is
the recompiled code itself - blocks of a few instructions that each write
their registers back and return to a dispatcher - at over half the thread.

**"At 1x ours is far more pixelated than footage of the game."** Two
things, neither a fault in the picture. The owner's `gc_prefs.ini` had the
window's Scale Filter on Nearest, which shows each of the console's pixels
as a square three across on a 1440-line screen; Linear is the default. And
the footage was not drawn at the console's size: the same scene here at
four times it has the footage's detail (the castle's railings, the text's
edges), and at 1x our frame is the 4x one averaged down, a tenth to a fifth
softer (the display copy's vertical filter, as on a console). The footage
is also 16:9, which this does not offer: the console's setting is fixed at
4:3 (`WiiSettings.widescreen`) and the window is 4:3.

**"Things get blurry the closer they are to Mario."** It was not blur, and
it only shows above 1x. The prologue's depth of field is the picture copied
to a texture and drawn back over everything further than the focus, which
follows Mario; a copy kept on the card was the console's 640 pixels
whatever the picture was drawn at, so at 4x whatever stood at or behind the
focus - Mario, the Toads beside him, the hill - came back in blocks of four
pixels behind a sharp foreground. The owner's save state of it found it in
one run. Copies kept as textures are now as large as the picture
(`docs/GC-PLAN.md`, "A copy as large as the picture"). That scene is also
the heaviest yet - 22,000 triangles a field, six copies, 13.4 ms of the
machine's thread - which is the lag the owner saw there: late fields, none
of them long enough to break the sound.

A copy kept as a texture is also found again by its bytes now, after a game
has used the place for something else and written its copy back
(2026-10-09; `docs/GC-PLAN.md`, "A copy found again by its bytes", which
Shrek 2's pause screen needed). This game never asks for it: every copy it
draws with still stands, in play and on its own pause screen, and at 3x
every picture of 600 fields of the owner's heaviest scene and of 500
through the pause screen is the same with it and without. What it pays is
three buffers changing hands a field, and 0.6 MB a field kept as copies
land that was dropped before.

**"Work on the lag."** (2026-10-09, with a save state of a heavier scene
still: 30,000 triangles a field, 16.0 ms of the machine's thread.) The game
itself idles 58 % of that field; the cost is ours, 3 ns an instruction, and
flat across the program. What was in reach was the present, which ran after
the field on the same thread and cost 0.8 to 2.5 ms: a field now runs on
its own thread and the picture before goes on screen meanwhile
(`docs/GC-PLAN.md`, "A field on its own thread, and a present in two
halves"). At 1x and 2x that scene no longer breaks the sound. At 3x, the
owner's setting, it still does: the graphics card is 95 % busy there, which
is the fast path's fragments and not yet looked at.

**"Pushed the start button and it froze."** The game had crashed, and
what looked like a freeze was its own exception handler: a thread of
priority 0 in a loop that waits fifty milliseconds by the clock and reads
the remotes, looking for the button sequence that shows a retail game's
crash screen, while the thread that draws never runs again. The owner made
a save state of it, which is what found it: the thread list out of a memory
dump (`threads.py` in the session's scratch) had the main thread stopped at
a load through a pointer that was two pixels of a picture, and the object
the pointer came from, and two more before it, were under RGB565 that
decoded as the top left of the scene. Holding + for a third of a second
opens the pause screen, which copies 320 by 228 of the framebuffer into a
160 by 114 texture with the halving bit - the first copy this game makes
with it - and a halved copy here was half as wide and no less tall: 228
rows written where the game had made room for 114. It is half as tall now
(`out_rows`, `copy_row` in `gc_efb.c`, the card's copy program beside it);
`gc_gx`'s "a halved copy" holds the size, the mean of four and the bytes
after the texture. Every renderer had it. A press of six fields does not
open the pause screen, which is why the first scripted tries found nothing.

Also found: at four times the console's size the game runs at two thirds
of its speed in a window. The machine's thread is not the limit there (14
ms); a peek waits 8 ms a field for the frame before, because every display
copy brings a 2560 by 1824 picture back from the card for the window and
the readback thread cannot keep up (45 ms a copy). The window should be
given the card's picture without the round trip.

**Save states** came out of this too, because every capture was a replay
from the console being switched on. A state now carries the Wii's half of
the machine (`WiiState`; MEM2 is the tail of the memory section) and the
flash filesystem with it, `/tmp` included, as the memory card is carried
for a GameCube; a load keeps the flash it replaces as `<file>.nand.bak`.
Held by `wii_fs_device`'s save-state test, and by the game: saved at field
4,700 and resumed, it ends field 5,400 at the hash and the frame of the run
that went straight through. The runner also has `--dump-frame-from FIELD`,
and `GC_GUEST_PROFILE_RAW=FILE` writes the profiler's samples for a link
map to name.

## A 16:9 television (2026-10-08)

The owner's reference for how the game should look was 16:9, and the
console here was fixed at 4:3. A game does not choose: it reads the
console's setting - `IPL.AR` in SYSCONF - when it starts, draws its 640
pixels for whichever shape that names, and the television stretches them.

- **The setting is the console's.** `wii_nand_set_widescreen` changes that
  one byte of SYSCONF and nothing else in the file (a game keeps the
  remotes it has paired there), and nothing at all when the shape is
  already the one asked for; `wii_console_set_widescreen` does it for a
  machine's flash.
- **The window is 16:9 unless told otherwise.** "Widescreen (16:9)" under
  the Wii's controls in the menu, `wii.widescreen` in `gc_prefs.ini`, on by
  default; `--wii-aspect 4:3|16:9` for one run, on the window and on the
  headless runner (which leaves the console at 4:3 unless asked, so every
  frame and hash it was held to stands). It is written before the game
  starts and takes effect the next time the game is started.
- **The window shows the shape the running game was started in**, read
  back from the console's settings (`wide_now`), not the setting: a state
  saved in 4:3 loads into a 4:3 window whatever the menu says, because the
  state carries its flash. Pixel Perfect scaling keeps whole lines; across,
  a 16:9 picture's pixels are a third wider than tall and cannot be whole.
- **The pointer needed nothing.** It is a fraction of the picture either
  way: the game's hand is drawn at the same fraction of the frame for the
  same scripted pointer in both shapes.

Super Mario Galaxy 2 in 16:9: the strap screen and the storybook keep
their shape inside a wider frame, the title, the file select and the game
itself show more to either side, and a planet that is an oval in the 640
pixels is round in the window (853 by 480 at 1x, 1707 by 960 at 2x). Held
by `wii_nand`'s picture-shape test; the window's part was looked at, not
tested.

## What the instruction test found

`wii_cputest` is Andrew Church's test of every 750CL instruction, validated
on the real processor. The GameCube backend's own vectors were measured on
an emulator; this is the first hardware-validated evidence the core has
had, and it disagrees with it in three ways.

- **A real bug, fixed.** `lswi` and `lswx` cleared the register after the
  last one they filled whenever the byte count was a multiple of four. A
  32-byte load into r25 wraps to r0, and the stack pointer went with it -
  which is how it was found: the test never came back. Fixed in
  `gc_load_string`, with a unit test.
- **268 results that differ**, recorded as the program's baseline. By
  instruction: `fctiw` (20), the paired-single reciprocal, divide and
  reciprocal-square-root estimates (31), `fres`, `fdiv`, the
  multiply-add family in both single and paired forms, the compares and
  the FPSCR moves, and six quantised stores (`psq_st` with a scale).
  Nearly all are in what a floating-point operation leaves in FPSCR -
  exception and summary bits, and results when an exception is enabled -
  rather than in the values games compute with. The program prints each
  one with its address.
- **Undocumented forms stop the core.** A 750CL executes an instruction
  with a stray reserved bit set (the first in the test is `mulhw` with its
  overflow bit) by ignoring the bit. The decoder calls it not an
  instruction and stops. The test's undocumented-form section is turned
  off until the decoder accepts them.

None of this is the Wii's: the processor core is shared, and a GameCube
game sees the same. It is listed in `TODO.md`.

## How the machine becomes a Wii

- `gc_console_select(1)` before the first `gc_machine_create()` sets three
  process-wide figures: the processor's clock (729 MHz), core cycles per
  video clock (27) and the size of main memory (24 + 64 MB). They are
  process-wide because they are compiled into loops throughout the runtime
  and because `GcMachine` must not grow.
- Main memory is one host block, MEM2 straight after MEM1. Everything that
  reads guest memory by offset (the command stream, textures, DMA) uses
  offsets into that block and one bound, `gc_ram_size`. Register values
  become offsets through `gc_ram_from_register()`, which on a GameCube is
  the 26-bit mask it always was.
- `GcCpu::machine` points at a `WiiState`: Hollywood's registers, the
  mailbox, the interrupt controller, HID4 and BAT4-7, and IOS's descriptor
  table and queue. It is plain data so that a save state can carry it.

## Known deviations

- `wii_cputest`: 268 instruction results differ from a 750CL's, and the
  undocumented-form section is off ([What the instruction test
  found](#what-the-instruction-test-found)). Baseline 268.
- hwtests on the Wii machine has the GameCube's deviations and no others:
  `cputest_pairedmove` 388, `cputest_reciprocal` 1, `gxtest_clipping` 4.

## Open items

- **A retail game played.** Super Mario Galaxy 2 gets into its prologue
  from a script ([What the first retail game found](#what-the-first-retail-game-found));
  it has not been played by hand, and P6's gates (thirty minutes, the
  native share, differential, speed) have not been run.
- **Speed.** In a window, on the fast OpenGL renderer, the script above
  runs its 6,000 fields at 96% of a console's speed, and at 59.7 to 59.9
  fields a second everywhere but the two stretches where the game loads
  (RTX 5070 Ti, 2026-10-07). It was 74%, and half speed in the prologue,
  before the waits in `docs/GC-PLAN.md`, "The waits that kept three
  threads in step", were taken out. The prologue's field takes the
  machine's thread 14 ms of 16.7, which is not much room: a heavier scene
  will not hold. The software renderer is far slower there (over 100 ms a
  field). The exact OpenGL path ran the script's first 3,000 fields, to the
  storybook, in 47 s with nothing pacing it and the right picture at the
  end; it has not been timed in a window or in the prologue.
- **The headless runner and the windowed frontend run different machines.**
  The same script gives a different state hash from the second field on.
  Something is set up differently (a memory card in the slot, settings);
  it has not been looked for, and until it is a script's result in one says
  less about the other than it should.
- **Sound.** The Zelda microcode mixes (`docs/gc-dsp-zelda.md`); what is
  left of it is listed there, and only its title music has been compared
  with anything. AX as Wii games use it is not written; a first step (the
  GameCube voice code split so a second command set can share it) is parked
  on a branch, with only the two `gc_ax` tests run on it.
- **Relocatable modules.** `gcrecomp` compiles the disc's main executable.
  Many Wii games keep most of their code in `.rel` modules that are loaded
  and relocated at run time (GC-PLAN.md planned for them; Shrek 2 has none,
  so they were never built). Such code runs interpreted today - correct,
  and several times slower. Recompiling a module needs its code keyed by
  what is in memory rather than by a fixed address (the NES backend's
  lesson), or a trace of where the game loads it plus a check of the bytes
  before running natively.
- A program that exits in the middle of a field makes `--differential`
  report a divergence (`finished` differs) and then search for a block for
  minutes. That is the runner's, not the Wii's; give it a field count below
  the program's end.
- libogc clears MEM1 above its own arena, so the filesystem table an
  apploader leaves at the top of MEM1 does not survive into a libogc
  program. A retail game's library reads it first; nothing tests that yet.
- Tickets and title metadata are not written into the flash filesystem
  when a disc is verified, as a console does; nothing has needed them.
- 16:9 is the window's and the console's (below); the hub has no setting
  for it, and a cover or a screenshot of a 16:9 game is the console's 640
  pixels across (that times the internal resolution for a screenshot taken
  with the key), a quarter narrower than the picture.
- The larger picture reaches the window by way of main memory; at 4x that
  is the game's speed limit (above).
- Saving a state from the window's keys has not been tried on a Wii game:
  states were made by the headless runner (`--save-state`) and loaded by
  it and by the window (`--load-state`).
- Coverage and the profiler index MEM1 only; code a game loads into MEM2
  (REL modules) is not counted.
- The reset and power buttons are never pressed (the hook is held).
- `ES_LAUNCH` ends the run rather than reloading IOS.
