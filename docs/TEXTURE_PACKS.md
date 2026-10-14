# Texture packs

A texture pack replaces a Game Boy game's tile art with higher-resolution
images while the game runs. Replacement happens only in the host picture:
guest memory, registers, timing, saves, and the accurate 160x144 framebuffer
are unchanged.

```bash
./output/game/build/game --texture-pack game-hd.gbtp
```

The runtime API is `runtime/include/gbrt_texture_pack.h`. The authoring tool
is `tools/gb_texture_pack.py`, which uses only the Python standard library.

## How replacement works

While a pack is active, the accurate PPU records five things for every pixel
it emits:

- the layer (background, window, or sprite);
- the VRAM tile it read;
- the tile column and row it sampled;
- the flips; and
- the palette in effect.

When a frame completes, the pack snapshots that record together with the tile
data.

To show the frame, each pixel's tile is identified by the SHA-256 of its 16
bytes of 2bpp data and looked up in the pack.

- **Tile found:** the pixel becomes the matching `scale`x`scale` block of the
  replacement image, mirrored when the tile was drawn flipped.
- **Tile missing, blank, or LCD off:** the pixel stays the original color,
  scaled.

Replacement pixels with partial alpha blend over that original color.

Because the record comes from the PPU as it draws, mid-frame scrolling,
window splits, sprite priority, and the ten-sprites-per-line limit all carry
over exactly. The layer is recorded but not yet used for lookup.

The SDL frontend shows the composite in place of the normal texture.
`--dump-frames` screenshots are the composite as well, at `160*scale` by
`144*scale`.

## Keys and palettes

A tile is keyed by its pixel data, not its VRAM position. The same art keeps
its replacement wherever the game loads it, and an animation frame is simply a
different tile.

Each entry applies to one palette selection:

| `palette` in pack.json | Applies to |
|---|---|
| `"any"` | every palette |
| `{"dmg": 228}` | DMG (and CGB compatibility mode) pixels drawn with BGP/OBP value `0xE4` |
| `{"cgb": 3}` | CGB pixels drawn with palette number 3 |

When both exist for a tile, the palette-specific entry wins. Palette-specific
entries let one tile have different art where a game reuses it with different
shades, for example a flashing enemy.

## Authoring workflow

1. **Collect screens.** Save states where the art you want is on screen; the
   `run` command in [DISCOVERY.md](DISCOVERY.md) writes one. Every animation
   frame is its own tile, so capture more than one moment.

2. **Export a template.**

   ```bash
   python3 tools/gb_texture_pack.py template \
     --state town.state --state cave.state --rom game.gb \
     --out game-hd --scale 4 --pack-id org.example.game-hd --license CC-BY-4.0
   ```

   This writes `game-hd/pack.json` and one `8*scale`-pixel PNG per distinct
   tile visible in those states: sprites plus the background and window under
   the end-of-frame scroll registers. Each PNG is the original tile scaled up,
   ready to paint over. Blank and single-color tiles are skipped. Rerunning
   with more states adds new tiles and keeps existing entries and images.
   `--per-palette` writes one entry per tile and DMG palette instead of
   `"any"`.

3. **Paint.** Edit the PNGs at the same size. RGBA, RGB, grayscale, and
   indexed 8-bit PNGs are accepted. Delete entries you do not replace; missing
   tiles fall back to the original art.

4. **Build and run** from the command line, or add the pack to a game in the
   [Hub](../hub/README.md) with *Texture pack → Add pack...*.

   ```bash
   python3 tools/gb_texture_pack.py build game-hd/pack.json --out game-hd.gbtp --rom game.gb
   ./output/game/build/game --texture-pack game-hd.gbtp
   ```

   `inspect game-hd.gbtp` lists the entries a built pack contains.

`gb_discover.py tiles` reports the same `tile_sha256` keys for the sprites
around a position, which helps find a specific character's tiles in a
template.

## pack.json

```json
{
  "schema": "gbrecomp.texture-pack",
  "version": 1,
  "pack_id": "org.example.game-hd",
  "license": "CC-BY-4.0",
  "rom": {"sha256": "64 lowercase hex digits", "size": 1048576},
  "scale": 4,
  "tiles": [
    {"tile_sha256": "64 lowercase hex digits", "palette": "any", "image": "tiles/0123456789abcdef.png"}
  ]
}
```

`build` fails on any of these:

- the wrong schema or version;
- a pack ID or license that is not a short identifier;
- a scale outside 1-8;
- an image of the wrong size, missing, or outside the pack directory;
- a malformed hash or palette;
- a duplicate tile and palette pair;
- more than 4,096 entries; or
- with `--rom`, a ROM that does not match the manifest.

## .gbtp format

Little endian. The authoritative description is in `gbrt_texture_pack.h`.

| Part | Size | Contents |
|---|---|---|
| header | 184 bytes | magic `GBTP`, version 1, ROM SHA-256 and size, scale, entry count, reserved zero, pack ID and SPDX license (64 bytes each, NUL terminated) |
| entries | 36 bytes each | tile SHA-256, palette kind (0 any, 1 DMG, 2 CGB), palette, two reserved zero bytes; strictly ascending |
| images | `(8*scale)^2 * 4` bytes each | RGBA rows, top to bottom, in entry order |

The runtime rejects a pack before activating it if:

- the magic, version, or size is wrong;
- the ROM differs;
- the limits are exceeded;
- entries are unsorted or duplicated;
- reserved fields are nonzero; or
- the strings are missing their terminator.

A rejected `--texture-pack` stops the executable instead of silently running
without it.

## Costs

Measured on Link's Awakening, Windows, Release build:

- **No pack loaded:** PPU instrumentation costs nothing measurable; headless
  speed is within run-to-run noise.
- **Pack loaded, headless:** recording pixel sources costs about 9%
  (20,000 frames in 5.6 s instead of 5.15 s).
- **Pack loaded, windowed:** snapshot plus composition at scale 4 takes about
  0.5 ms per frame with 83% of pixels replaced.
- **Determinism:** savestates after identical runs are byte-identical with and
  without a pack.

## Limits

- **Mid-frame tile writes.** Tile data is snapshotted when the frame
  completes. A game that rewrites tile data mid-frame shows the end-of-frame
  art for pixels drawn earlier; LCD-off tile writes during VBlank are
  unaffected.
- **Templates assume DMG palettes.** Savestates do not record the hardware
  model, so `--per-palette` on a CGB game writes DMG register keys. Use
  `"any"`, or change those entries to `{"cgb": N}`.
- **Tiles only.** Replacement is per tile. Effects that span tiles, larger
  sprites made of several tiles, and widescreen are separate work (see
  [NATIVE_PRESENTATION.md](NATIVE_PRESENTATION.md)).
- **One pack at a time**, and not in `--differential` mode. Packs are not
  included in *Compile for export* output.

# Game Boy Advance texture packs

GBA games take `--texture-pack FILE.gbatp` too (the SDL game and the
headless runner, where `--dump-frame` then writes the composed frame). The
runtime API is `runtime-gba/include/gba_texture_pack.h`; the build tool is
`tools/gba_texture_pack.py` (standard library; Pillow speeds up PNG
decoding); packs are made from recordings of play with the pipeline in
[tools/gba/texpack](../tools/gba/texpack/README.md), which upscales whole
background layers and sprites (xBRZ, MMPX or an AI model) and cuts them
into tiles.

## How GBA replacement works

While a pack is active the PPU records, for every pixel, the tile pixel of
the top layer and of the layer under it: the tile's VRAM address, the
tile-data column and row sampled, flips, palette base. It also records their
raw colours, the colour effect applied (alpha blend, brighten, darken), and
the top layer's depth. At the end of the frame the record, VRAM and palette
RAM are snapshotted.

Composition rebuilds the frame at the pack's scale:

- Each tile is looked up by the SHA-256 of its VRAM bytes (32 for 4bpp, 64
  for 8bpp) and of its palette RAM bytes (16 or 256 colours).
- **No exact palette:** an entry for the same tile whose palette maps onto
  the current one by a per-channel linear change (worst error at most one
  5-bit step) is recoloured by that change. This covers fades and flashes
  without an image per fade step.
- Replacement pixels with partial alpha blend over the layer under them,
  then the recorded effect is applied the way the PPU applied it.
- **Edges:** where a neighbouring pixel belongs to a layer in front, the half
  of this pixel next to it samples that layer's image beyond its original
  edge, so upscaled outlines are not cut back to the original pixel grid.
- **Kept original, scaled:** affine and bitmap layers, rotated or scaled
  sprites, mosaic larger than 1x1, the backdrop, and tiles without an image.

## .gbatp format

Little endian. The authoritative description is in `gba_texture_pack.h`.

| Part | Size | Contents |
|---|---|---|
| header | 192 bytes | magic `GATP`, version 1, ROM SHA-256 and size (all zero: any ROM), scale, entry count, palette data size, pack ID and license (64 bytes each, NUL terminated), two reserved zero words |
| entries | 76 bytes each | tile SHA-256, palette SHA-256, bits per pixel, three reserved zero bytes, offset of the palette bytes, reserved zero; strictly ascending by the two hashes |
| palette data | as declared | the palettes the entries were made with |
| images | `(8*scale)^2 * 4` bytes each | RGBA rows in entry order |

Up to 65,536 entries. pack.json for the build tool:

```json
{
  "schema": "native-console.gba-texture-pack",
  "version": 1,
  "scale": 4,
  "pack_id": "org.example.game-hd",
  "license": "CC-BY-4.0",
  "tiles": [
    {"tile_sha256": "64 hex digits", "bpp": 4, "palette_sha256": "64 hex digits",
     "palette": "hex of the 32 or 512 palette bytes", "image": "tiles/....png"}
  ]
}
```

## GBA costs

Measured on The Minish Cap, Windows, Release build:

- **No pack:** the PPU checks one pointer per line.
- **Pack loaded, headless interpreter:** 3,000 frames in 3.36 s instead of
  2.77 s (recording, plus loading a 77 MB pack); frame hashes are identical
  with and without the pack.
- **Composition at scale 4:** 2-3 ms per frame (an xBRZ pack of 18,434
  tiles, 130-370 distinct tiles on screen).
- **Recording textures** (`--record-textures`): one ~100 KB record per 10
  frames while the picture changes.

## GBA limits

- Tile data and palettes are snapshotted at the end of the frame, like the
  Game Boy packs, so mid-frame palette or tile changes use end-of-frame art.
- Only what the recordings saw is upscaled; everything else stays original.
- Packs are not included in *Compile for export* output.
