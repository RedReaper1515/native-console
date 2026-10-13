# Game Boy Color support

Native Console runs CGB-only cartridges and CGB-enhanced cartridges through the same generated-project flow used for DMG games. CGB support is functional but not complete; compatibility claims should remain tied to fresh tests and reproducible game-specific evidence.

## Implemented

- automatic hardware selection from cartridge header byte `0x143`
- explicit `--model auto|dmg|cgb` runtime selection
- CGB mode and DMG-on-CGB compatibility mode
- KEY1 speed switching and double-speed CPU operation, including half-system-cycle carry across split CPU bus phases
- VBK and SVBK bank selection
- CGB palettes and VRAM-bank-1 tile attributes
- CGB background, window, and sprite rendering
- timed general-purpose HDMA and HBlank HDMA
- model- and source-aware OAM DMA behavior
- MBC3 RTC persistence and stable battery-save identities
- 9-bit MBC5 ROM banking through analysis, code generation, runtime mapping, and interpreter fallback

## Current hardware-test snapshot

The curated CGB selection was rerun on 2026-09-10:

| Test | Result |
| --- | --- |
| `boot_regs-cgb` | Pass |
| `boot_div-cgbABCDE` | Pass |
| `unused_hwio-C` | Pass |
| `vblank_stat_intr-C` | Pass |
| `boot_div-cgb0` | Fail |

`boot_div-cgb0` is expected to fail. The CGB revision 0 boot ROM hands off
about 130 M-cycles later than revisions A–E, so no single post-boot state
passes both `boot_div` tests; the runtime models A–E.

Blargg `interrupt_time` also passes both its normal-speed and CGB double-speed 13-M-cycle interrupt measurements. It is tracked in the complete catalogue rather than the five-case curated CGB subset above.

The passing boot-register test validates the runtime's configured post-boot state; it does not mean the CGB boot ROM is emulated. See [Accuracy](ACCURACY.md) for the complete configured suite.

Tetris DX received a fresh generated-project optimization smoke on 2026-07-13. Generated and interpreted execution matched for 500,000 steps, and the stable-span and scalar PPU paths produced the same 1,800-frame state SHA-256, `4bf4c98fec004470ea680ce89e27aa6971fccd232d6b7ca0fa84076c58431bdf`. The generated workload still records 545 interpreter fallbacks over 1,800 frames, so this is CGB behavior/performance evidence rather than a fallback-free analyzer-coverage claim. See [the NR-1 through NR-3 result](docs/NR123_DYNAMIC_OPTIMIZATION_RESULTS_2026-07-13.md). That state hash predates the 2026-09-10 post-boot DIV model, which changes CGB start-up state, so it no longer reproduces.

## Post-boot DIV

Generated executables start from a post-boot state instead of running the CGB
boot ROM, and that boot ROM runs for a header-dependent time, so DIV at the
`$0100` hand-off has no single value. The runtime computes it from the header
(`ppu_cgb_boot_header_mcycles` in `runtime/src/ppu.c`):

- Header-dependent work happens before the LCD is switched on (licensee
  check, title-checksum search, fourth-letter tie-break, palette shuffle) or
  after the final VBlank wait (the KEY0 and compatibility-palette tail).
  Everything in between is paced by LCD frames, so each of these moves the
  hand-off by its own length. Cycle counts come from ISSOtm's disassembly of
  the CGB boot ROM (gb-bootroms `src/cgb.asm`, commit `443d7f0`).
- One base constant is calibrated on `boot_div-cgbABCDE`, which was verified
  on CGB A–E hardware and pins the hand-off to a single M-cycle. It uses the
  same sub-M-cycle convention as the DMG value `$ABCC`, which sits at the same
  edge of the `boot_div-dmgABCmgb` window.
- Only that DMG-cartridge, non-Nintendo-licensee path is hardware-verified.
  CGB cartridges skip a per-frame joypad poll during the logo animation, which
  moves the final VBlank poll by either +2 or −4 M-cycles; the model takes +2.
  Buttons held during the logo are not modelled.

The typical results agree with mGBA's fixed skip-BIOS DIV bytes: `$26` for a
non-Nintendo DMG cartridge and `$2F` for a Nintendo-licensed CGB cartridge,
whose title is not in the palette table.

## Known gaps

- the CGB boot ROM is not emulated; post-boot DIV models revisions A–E only,
  and its CGB-cartridge path has no hardware test of its own
- unused-bit readback is hardware-tested in DMG compatibility mode
  (`unused_hwio-C`) but not in CGB mode
- KEY0/PGB edge behavior is incomplete
- FF56 infrared support is stub-level
- serial behavior is sufficient for current single-system use but is not a complete link-cable implementation
- double-speed, HDMA, LCD/STAT, and DMG-on-CGB interactions still need
  broader independent validation on real game routes
- known-good games can still encounter analyzer coverage gaps and interpreter fallback

## Reproduce the curated subset

Build the current recompiler, then run the five configured `misc` cases:

```bash
cmake -G Ninja -B build .
ninja -C build
python3 tools/run_tests.py --filter misc --rebuild
```

The test runner treats build errors, timeouts, missing state dumps, incomplete runs, and an empty selection as failures.

## Reference order

For CGB implementation work:

1. consult `tech_docs/pan_docs.md`
2. compare with the local `SameBoy/` implementation when Pan Docs is ambiguous
3. add or update a repository-owned regression test
4. run the relevant Mooneye CGB case and a recorded real-game smoke
