# GBA texture-pack pipeline

Makes a high-resolution texture pack for a generated GBA game by upscaling
the art the game actually draws. The runtime side (recording, the `.gbatp`
format, composition) is described in [TEXTURE_PACKS.md](../../../docs/TEXTURE_PACKS.md).

1. **Record.** Play the game with `--record-textures FILE`. Every 10 frames,
   when the picture changed, it appends the I/O registers, palette RAM, VRAM
   and OAM (about 100 KB a record). The headless runner takes the same flag
   plus `--record-every N`, for scripted runs:

   ```bash
   game.exe --record-textures town.bin
   game.exe --frames 30000 --auto-input --record-textures intro.bin
   ```

   Only art that was on screen gets upscaled, so play through the areas the
   pack should cover. Records from several sessions can be passed together.

2. **Upscale and cut into tiles.**

   ```bash
   python build_pack.py xbrz pack-dir town.bin intro.bin --pack-id org.example.hd --license CC-BY-4.0
   ```

   Each background layer (its whole map) and each sprite is upscaled as one
   image, so tiles are upscaled beside their real neighbours; the result is
   cut back into 4x tiles. A tile drawn in several places keeps the version
   that occurs most often (or the one closest to the rest). Palettes that are
   a per-channel linear change of one already kept (fades, flashes) are not
   stored: the runtime recolours. `--preview FILESTEM:FRAME` rebuilds recorded
   frames from the pack's tiles as PNGs to check the result.

3. **Build the pack** with the standard-library tool, tied to the ROM:

   ```bash
   python tools/gba_texture_pack.py build pack-dir/pack.json --out game-hd.gbatp --rom game.gba
   game.exe --texture-pack game-hd.gbatp
   ```

   In the Hub, add it to the game with *Texture pack → Add pack...*.

Tiles can be repainted by hand between steps 2 and 3: edit the PNGs in
`pack-dir/tiles` at the same size (Aseprite and similar editors work).

## Requirements

Python 3 with numpy and Pillow. The upscalers load native code:

| Method | Needs |
|---|---|
| `nearest` | nothing |
| `mmpx` | `runtimes/runtime-gba/src/gba_upscale.c` built as a shared library, path in `GBA_MMPX_LIB` |
| `xbrz` | xBRZ (GPL-3, not included) built with `xbrz_c.cpp`, path in `GBA_XBRZ_LIB` |
| `ai` | PyTorch with CUDA, `spandrel`, and an ESRGAN-family model file in `GBA_UPSCALE_MODEL` |

MSVC build commands, from a Developer Command Prompt:

```bat
cl /O2 /LD /I runtimes/runtime-gba\include runtimes/runtime-gba\src\gba_upscale.c /link /EXPORT:gba_mmpx_scale2x /OUT:mmpx.dll
rem xBRZ 1.9 from https://sourceforge.net/projects/xbrz/ unpacked beside xbrz_c.cpp:
cl /O2 /EHsc /std:c++20 /LD xbrz.cpp xbrz_c.cpp /Fe:xbrz.dll
```

xBRZ's GPL applies to that library and to packs' build process only as a
tool; nothing from it is linked into generated games.

`extract.py ROM OUTDIR RECORDS...` is for exploring a game's art: it unpacks
LZ77-compressed graphics from the ROM, finds uncompressed tile regions the
game drew from, and writes sheets coloured with the palettes seen in the
recordings.
