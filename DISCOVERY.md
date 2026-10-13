# Finding what a game's memory and graphics mean

`tools/gb_discover.py` names a Game Boy game's variables and graphics by
running experiments on its generated executable. The results feed
`gbrecomp --symbols`, native patches, and replacement art.

Each experiment starts from a runtime savestate, runs the executable headless
with scripted input, and reads WRAM, HRAM, OAM, and VRAM out of the savestate
the runtime writes on exit. A candidate address is only a guess until an
edited savestate makes the game behave differently.

The tool uses only the Python standard library.

## Rules

- **Your save is never touched.** Every run gets a fresh `--save-dir` under
  `--work`. `--save-file` copies a battery save into each run; the original is
  only read.
- **Frames count from the loaded state.** `--frames 100 --input 20:R:40`
  means: load, wait 20 frames, hold Right for 40, stop at frame 100. Start
  input a few frames after the load; the first frames can miss a press.
- **Confirm by consequence, not by display.** Games often redraw HUDs only
  when their own code changes a value. Link's Awakening ignores a poked health
  byte on the HUD, but a poked 0 kills Link.

## Getting a starting state

```bash
python3 tools/gb_discover.py run --exe game.exe --work scratch \
  --save-file game.sav --frames 1000 --input "300:S:5,500:S:5,700:A:5" \
  --shots 999 --out-state start.state
```

Check `scratch/run/shot_00999.png`, adjust input until the player is in
control, then branch every experiment from `start.state`.

## Commands

| Command | Finds | How |
|---|---|---|
| `input-scan --axis R:L --axis D:U` | player position | bytes that rise the longer one button is held and fall with the other; a separate list covers a wall blocking one direction |
| `sprite-track --ignore-near XADDR,YADDR` | entity position tables | bytes that equal a moving sprite's screen position plus a constant, excluding sprites near the player |
| `table-sweep --range C200:C600 --step 16` | entity state/active flags | writes 0 to slot 0 of each 16-entry table and reports which one removes the sprite |
| `poke --set ADDR=VALUE --watch ADDR --shots N` | proof | runs the original and edited state side by side and writes both screenshots |
| `watch --frames 70:100:10 --addr ADDR` | event effects | values over time under one input script, e.g. health while walking into an enemy |
| `peek`, `diff` | anything | hex dump of a state, or the bytes that differ between two states |
| `tiles --rom game.gb --near XADDR,YADDR` | graphics | exports sprite tiles as PNGs with per-tile SHA-256 keys, flips, palettes, and where the tile bytes occur in the ROM |

## Worked example: Link's Awakening

Found on 2026-09-13 from a save file standing in the overworld (USA/Europe Rev 2):

| Meaning | Address | Evidence |
|---|---|---|
| Player X, Y | `FF98`, `FF99` | `input-scan`: X follows R/L exactly; Y follows U while a fence blocks D |
| Player health | `DB5A` | 8 units per heart; `watch` shows 24 → 22 → 17 → 16 on enemy contact; `poke --set DB5A=0` reaches Game Over |
| Entity X, Y tables | `C200`, `C210` | `sprite-track` (screen X + 7, screen Y + 15); moving them moves the enemy |
| Entity state table | `C280` | `table-sweep`: 0 removes the enemy; 5 is active |

Enemies, NPCs, and objects share one 16-slot entity table. The type table
(likely `C3A0`) tells them apart and still needs confirming, as does
per-entity health.

`tiles` then showed Link as two 8x16 sprites whose tile bytes appear in ROM
bank `0C`, and the enemy as one tile drawn twice with an X flip, stored in
bank `11`.

## Naming what you found

Write confirmed addresses into an RGBDS-style symbol file:

```text
00:DB5A wPlayerHealth
00:FF98 hPlayerX
00:FF99 hPlayerY
00:C200 wEntityXTable
00:C210 wEntityYTable
00:C280 wEntityStateTable
```

Regenerating with `gbrecomp game.gb --symbols found.sym -o out` turns
`gb_write8(ctx, GB_WRAMX_BASE + 0x0b5a, ctx->a)` into
`gbrt_timed_bus_write8(ctx, GB_ADDR_wPlayerHealth, ctx->a, 15)`, and ROM
symbols name generated functions (`sym_<name>`). Name a function only once you
know what it does; writing a variable is not enough.

## Limits

- Only behaviour you can trigger and observe is found. Hidden rules, unused
  data, and code paths the script never reaches stay unnamed.
- Tables indexed through `HL` show up in generated C as `ctx->hl` accesses,
  not as named addresses; the names apply where the game uses the address
  directly.
- `tiles` reads tiles as they sit in VRAM. Compressed graphics do not appear
  byte for byte in the ROM, so `rom_locations` is empty for them.
- Each tile's SHA-256 identifies its content independent of palette. It is
  the key [texture packs](TEXTURE_PACKS.md) use to replace that tile's art.
