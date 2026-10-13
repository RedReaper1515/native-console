# Voxel view

A generated Game Boy executable can draw a tile-based game's playfield in 3D.
The game still runs exactly as before; the view is a host-side rendering of
each finished frame, like a [texture pack](TEXTURE_PACKS.md), and never
changes guest memory, timing, or saves.

```bash
game.exe --voxel-profile links_awakening.voxel
```

**F4** switches between 3D and the original picture. The Esc menu has a
*3D View* checkbox and *3D Tilt* / *3D Distance* sliders.

## In the Hub

Game Boy games have a **3D view** picker under the texture pack picker.
Profiles in `hub/voxel-profiles/` are compiled into the hub and offered as
*(built-in)* for the ROMs their `rom` lines name; choosing one copies it into
`games/<id>/voxel-profiles/`, where it can be edited. *Add profile...* imports
any `.voxel` file whose `rom` lines match the game (a profile without `rom`
lines fits any game). Copies of built-in profiles end with a
`# hub-builtin <hash>` line; when a newer hub ships a changed built-in, copies
nobody edited are updated, and *Reset to built-in* restores an edited one.
The choice is saved as `voxel_profile` in
`library.json` and passed as `--voxel-profile` on every launch, including
Steam shortcuts. Games built before the view existed show a rebuild notice.

## How it works

When a frame completes, the runtime snapshots VRAM, OAM, the I/O registers,
and the per-pixel sources the PPU recorded, then:

1. **Checks the profile's conditions** (`require` lines). If any fails, or the
   LCD is off, the host shows the normal 2D frame. Link's Awakening requires
   gameplay (`DB95` = `0B`) on the overworld (`DBA5` = `00`).
2. **Tracks the room grid.** The profile names the WRAM grid of room object
   IDs and the room-number byte. Objects are placed in the 16x16-cell space of
   the 256x256 tilemap at the current scroll position. When the room number
   changes by one column or row, the new room is placed next to the old one,
   and each cell switches to its new object as soon as the game redraws its
   tiles, so the 3D scene scrolls with the screen during transitions.
3. **Builds the scene.** Every playfield pixel becomes a column of voxels
   shaped by its object's class; sprites stand up as one-voxel-thick cutouts on
   their lowest row.
4. **Renders** with a small software rasterizer and depth buffer into a
   640x576 image (4x the Game Boy screen, same aspect ratio).
5. **Draws flat on top**: window-layer pixels (the HUD), rows below the
   playfield, and background cells whose tiles no longer match their room
   object, such as text boxes. When more than 35% of the visible cells no
   longer match (a menu screen), the frame falls back to 2D.

Rendering costs about 3-5 ms per frame on a desktop CPU.

## Profile format

Plain text, one directive per line, `#` starts a comment.

```text
voxel-profile 1
title Link's Awakening overworld   # name shown by launchers
rom ed42628d...63ad87       # SHA-256 (64 hex) of a ROM it is for; repeatable, none = any
playfield 160 128           # 3D area: full width, rows above the HUD
grid D711 10 8 16           # object grid address, columns, rows, bytes per row
room FFF6                   # room number; +-1 is a column step, +-16 a row step
require DB95 0B             # all must hold for 3D (hex address, hex value)
require DBA5 00
ground 3                    # ground thickness in voxels
camera 55 2.0               # tilt in degrees, distance in playfield heights
class grass relief raise=1 min_shade=2
class bush dome height=8 radius=5 groove=1 cutout=1
class building facade rows=1
object 04 grass             # object ID (hex) -> class
```

Addresses may be in WRAM (`C000-DFFF`), I/O (`FF00-FF7F`), or HRAM
(`FF80-FFFE`). Objects without an `object` line are flat ground.

Class shapes:

| Type | Shape | Parameters |
|---|---|---|
| `ground` | flat | |
| `relief` | pixels at `min_shade` or darker rise `raise` voxels | `raise`, `min_shade` |
| `sunken` | one voxel thick (water, holes) | |
| `box` | solid block | `height`, `cutout` |
| `dome` | rises to `height`, rounded over `radius` pixels from the class edge; the darkest shade sinks `groove` | `height`, `radius`, `groove`, `cutout` |
| `facade` | the bottom `rows` cells of each vertical run stand up as a front wall; the cells above become the roof on top of it | `rows` |
| `prop` | each separate object in the art (`span` cells square) becomes a box: art rows from `top_rows` down wrap all four sides, and the rows above are stretched over the top, on the body or on a slab `canopy` voxels thick | `span`, `top_rows`, `canopy` |

A `prop` object that covers several cells names its cell within the art:
`object 25 bigtree 0 0` is the top-left of a 2x2 tree, `object 28 bigtree 1 1`
its bottom-right. Link's Awakening uses it for fence posts (a cell holds two,
each with its round cap on top) and big trees (a canopy block on the trunk).

`cutout=1` keeps the lightest pixels that touch a cell's edge at ground level,
so a bush or post keeps its outline instead of filling its whole cell.

## Making a profile

`tools/la_voxel.py` builds the Link's Awakening profile and previews rooms as
MagicaVoxel `.vox` files:

```bash
python tools/la_voxel.py capture --exe game.exe --save game.sav --rooms 92,93,A2 --out states
python tools/la_voxel.py objects --state states/room_92.state --out objects.png
python tools/la_voxel.py scene --state states/room_92.state --out village.vox --preview
python tools/la_voxel.py profile --out hub/voxel-profiles/links_awakening.voxel
```

`objects` exports every object ID it sees as a picture sheet (cell row R,
column C is object `RC`); classify them in `tools/la_voxel_objects.json`. The
same table drives the `.vox` previews and the runtime profile. For a new game,
find its object grid and room byte with [`gb_discover.py`](DISCOVERY.md).

Set `GBRT_VOXEL_TRACE=1` to print per-frame render time, face count, and flat
overlay cells.

## Limits

- Only the visible playfield is 3D; there is no geometry beyond the screen
  edge.
- DMG games only: colors come from the four DMG shades.
- Unclassified objects are flat. The Link's Awakening table covers the Mabe
  Village area; the rest of Koholint still needs classifying.
- Indoor rooms, dungeons, and side-scrolling sections stay 2D.
- A savestate loaded while a text box is open treats the box's tiles as part
  of the room until the room changes.
