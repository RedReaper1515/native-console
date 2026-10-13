# GameCube Transform Unit (XF) Registers and Memory

## Provenance

Every fact in this document was derived by reading devkitPro/libogc's `gx.c` (zlib licence), the
GX implementation that shipped on real hardware for twenty years. Addresses and bit positions are
read directly out of the `GX_LOAD_XF_REG(addr, value)` / `GX_LOAD_XF_REGS(addr, n)` call sites and
out of the `GX_Set*` / `GX_Init*` functions that build their payloads. Where a layout is not
self-evident, the shifting expression from `gx.c` is quoted verbatim together with its line number,
so the claim can be re-checked against the source.

Dolphin's `XFMemory.h` (GPL-2.0, in `roms/gc/hwtests/gxtest/`) was **read as a cross-check only**.
No code, comment text, or wording from it has been copied. The copy of that header in this tree is
heavily trimmed — it contains **only** the `LitChannel` union — so it could cross-check exactly one
register family (§B.6, the colour/alpha channel control words) and nothing else. That is stated
explicitly wherever it matters.

Dolphin's `lighting.cpp` and `clipping.cpp` (same directory, same licence) were read to see which
registers a hardware test actually pokes; they corroborate `0x1005`, `0x1009`, `0x100a`, `0x100c`,
`0x100e`, `0x1010`, the viewport, and the projection. Again, no wording is reused.

Anything that could not be pinned down from `gx.c` is marked **Unverified:**. A missing field is
better than a wrong one.

Line numbers refer to the copy of `gx.c` used for this analysis (5199 lines,
`scratchpad/gx.c`). The helper macros are the standard libogc ones:

* `_SHIFTL(v,s,w)` — take the low `w` bits of `v` and place them at bit `s`.
* `_SHIFTR(v,s,w)` — extract `w` bits starting at bit `s`.

So `_SHIFTL(x,7,5)` means "a 5-bit field at bits 11:7".

---

## 1. FIFO encoding

The XF has a **single flat address space**. Addresses below `0x1000` are *XF memory* — matrices and
light blocks, written as arrays of 32-bit words. Addresses `0x1000` and above are *XF registers* —
individually meaningful control words. Both are reached through the same FIFO opcode.

### 1.1 Immediate write (`0x10`)

A write of *n* consecutive words starts with the opcode byte `0x10`, then one big-endian 32-bit
header, then *n* payload words. `gx.c:46-62`:

```c
#define GX_LOAD_XF_REG(x, y)			\
	do {								\
		wgPipe->U8 = 0x10;				\
		wgPipe->U32 = (u32)((x)&0xffff);		\
		wgPipe->U32 = (u32)(y);		\
	} while(0)

#define GX_LOAD_XF_REGS(x, n)			\
	do {								\
		wgPipe->U8 = 0x10;				\
		wgPipe->U32 = (u32)(((((n)&0xffff)-1)<<16)|((x)&0xffff));				\
	} while(0)
```

| Bits of the header word | Meaning |
|---|---|
| 15:0 | Destination XF address |
| 31:16 | Word count **minus one** (so `0x0000` means one word) |

`GX_LOAD_XF_REG` is the one-word case: it leaves bits 31:16 clear and writes the single payload
itself. `GX_LOAD_XF_REGS` only emits the header; the caller then pushes exactly *n* words into the
write-gather pipe (e.g. `WriteMtxPS4x3()` for a 3x4 matrix, `gx.c:2875`). Dolphin's test harness
uses a byte-identical macro (`cgx.cpp:63-68`), an independent confirmation of the header format.

**Consequence for a decoder:** the payload length is *not* implied by the address. A command-stream
decoder must read the count from bits 31:16 and consume that many words, even for addresses in the
register range. libogc writes the viewport as one six-word burst to `0x101a` (`gx.c:1784`) and the
projection as one seven-word burst to `0x1020` (`gx.c:1823`); it never writes those registers
individually.

### 1.2 Indexed write (`0x20`, `0x28`, `0x30`, `0x38`)

Four further opcodes load XF memory from an array in main RAM that was previously bound by a CP
array-base/stride register. Each is a single 32-bit word:

| Bits | Meaning |
|---|---|
| 11:0 | Destination XF address |
| 15:12 | Word count **minus one** (4 bits, so at most 16 words) |
| 31:16 | Index into the bound array |

The split is visible from the constants libogc ORs in, all of which occupy bits 15:12:

```c
wgPipe->U32 = ((_SHIFTL(mtxidx,16,16))|0xb000|(_SHIFTL(pnidx,2,8)));     // gx.c:2881
wgPipe->U32 = ((_SHIFTL(mtxidx,16,16))|0x8000|(0x0400|(pnidx*3)));       // gx.c:2899
wgPipe->U32 = ((_SHIFTL(mtxidx,16,16))|(_SHIFTL(size,12,4))|addr);       // gx.c:2928
reg = 0xf600|(_SHIFTL(idx,4,8));                                          // gx.c:4679
```

Reading the four literals: `0xb000` = 11 → 12 words (a 3x4 position
matrix), `0x8000` = 8 → 9 words (a 3x3 normal matrix), `size` = 7 or 11 → 8 or 12 words (a 2x4 or
3x4 texture matrix, `gx.c:2922`), `0xf000` = 15 → 16 words (a light block).

| Opcode | libogc function | Loads |
|---|---|---|
| `0x20` | `GX_LoadPosMtxIdx()` (`gx.c:2878`) | Position matrix |
| `0x28` | `GX_LoadNrmMtxIdx3x3()` (`gx.c:2896`) | Normal matrix |
| `0x30` | `GX_LoadTexMtxIdx()` (`gx.c:2920`) | Texture (or post-transform) matrix |
| `0x38` | `GX_LoadLightObjIdx()` (`gx.c:4643`) | Light block |

Note that the address field is only 12 bits here, which is exactly enough to cover XF *memory*
(everything below `0x1000`) and nothing else. Indexed loads cannot target XF registers.

### 1.3 Address index

| Address | Name | Section |
|---|---|---|
| `0x0000`–`0x00FF` | Position and texture matrix memory | [A.1](#a1-position-and-texture-matrix-memory-0x00000x00ff) |
| `0x0400`–`0x04FF` | Normal matrix memory | [A.2](#a2-normal-matrix-memory-0x0400) |
| `0x0500`–`0x05FF` | Dual / post-transform texture matrix memory | [A.3](#a3-dual--post-transform-texture-matrix-memory-0x0500) |
| `0x0600`–`0x067F` | Light memory, eight blocks of 16 words | [A.4](#a4-light-memory-0x0600) |
| `0x1000` | Error | [B.1](#b1-status-and-diagnostic-registers-0x10000x1007) |
| `0x1001` | Diagnostics | [B.1](#b1-status-and-diagnostic-registers-0x10000x1007) |
| `0x1002` | State 0 | [B.1](#b1-status-and-diagnostic-registers-0x10000x1007) |
| `0x1003` | State 1 | [B.1](#b1-status-and-diagnostic-registers-0x10000x1007) |
| `0x1004` | Clock | [B.1](#b1-status-and-diagnostic-registers-0x10000x1007) |
| `0x1005` | Clip disable | [B.2](#b2-clip-disable-0x1005) |
| `0x1006` | Performance metric select | [B.3](#b3-performance-metric-select-0x1006) |
| `0x1007` | *unassigned* | [B.1](#b1-status-and-diagnostic-registers-0x10000x1007) |
| `0x1008` | Vertex specs | [B.4](#b4-vertex-specs-0x1008) |
| `0x1009` | Number of colour channels | [B.5](#b5-number-of-colour-channels-0x1009) |
| `0x100A` | Channel 0 ambient colour | [B.7](#b7-ambient-and-material-colours-0x100a0x100d) |
| `0x100B` | Channel 1 ambient colour | [B.7](#b7-ambient-and-material-colours-0x100a0x100d) |
| `0x100C` | Channel 0 material colour | [B.7](#b7-ambient-and-material-colours-0x100a0x100d) |
| `0x100D` | Channel 1 material colour | [B.7](#b7-ambient-and-material-colours-0x100a0x100d) |
| `0x100E` | Colour channel 0 control | [B.6](#b6-colour-and-alpha-channel-control-0x100e0x1011) |
| `0x100F` | Colour channel 1 control | [B.6](#b6-colour-and-alpha-channel-control-0x100e0x1011) |
| `0x1010` | Alpha channel 0 control | [B.6](#b6-colour-and-alpha-channel-control-0x100e0x1011) |
| `0x1011` | Alpha channel 1 control | [B.6](#b6-colour-and-alpha-channel-control-0x100e0x1011) |
| `0x1012` | Dual-texture-transform enable | [B.8](#b8-dual-texture-transform-enable-0x1012) |
| `0x1018` | Matrix index A | [B.9](#b9-matrix-index-a-and-b-0x1018-0x1019) |
| `0x1019` | Matrix index B | [B.9](#b9-matrix-index-a-and-b-0x1018-0x1019) |
| `0x101A`–`0x101F` | Viewport, six floats | [B.10](#b10-viewport-0x101a0x101f) |
| `0x1020`–`0x1026` | Projection, seven words | [B.11](#b11-projection-0x10200x1026) |
| `0x103F` | Number of texture coordinate generators | [B.12](#b12-number-of-texture-generators-0x103f) |
| `0x1040`–`0x1047` | Texture coordinate generation info, texgen 0..7 | [B.13](#b13-texture-coordinate-generation-info-0x10400x1047) |
| `0x1050`–`0x1057` | Post-transform info, texgen 0..7 | [B.14](#b14-post-transform-texture-info-0x10500x1057) |

**Unverified:** every address not listed. libogc never writes `0x0100`–`0x03FF`, `0x0680`–`0x0FFF`,
`0x1007`, `0x1013`–`0x1017`, `0x1027`–`0x103E`, or `0x1048`–`0x104F`. The gaps between the memory
regions are almost certainly aliasing or padding, but nothing in either source proves it.

---

# A. XF memory (addresses below `0x1000`)

XF memory is addressed in **32-bit words**, and every matrix in it is stored as **rows of four
floats**. That single fact explains all of the addressing arithmetic below: a matrix *index* as it
appears in a vertex or in a matrix-index register counts rows, and the XF address is that index
times four.

## A.1 Position and texture matrix memory (`0x0000`–`0x00FF`)

`GX_LoadPosMtxImm()` (`gx.c:2872-2876`):

```c
GX_LOAD_XF_REGS((0x0000|(_SHIFTL(pnidx,2,8))),12);
WriteMtxPS4x3(mt,(void*)wgPipe);
```

`_SHIFTL(pnidx,2,8)` is `(pnidx & 0xff) << 2`, i.e. **address = index x 4**, and the burst is 12
words — three rows of four floats, a 3x4 affine matrix stored row-major.

The same index value is what `GX_SetCurrentMtx()` puts into the matrix index register
(`gx.c:2933`):

```c
__gx->mtxIdxLo = (__gx->mtxIdxLo&~0x3f)|(mtx&0x3f);
```

so the index is **6 bits**, 0..63, and the memory it can reach is `0x0000`–`0x00FF`: **64 rows of
four floats, 256 words total**. A 3x4 matrix occupies three of those rows, so at most 21 full
matrices fit, and the software convention (see below) uses 20 plus an identity.

**Texture matrices live in the same block, above the position matrices.**
`GX_LoadTexMtxImm()` (`gx.c:2902-2918`):

```c
if(texidx<GX_DTTMTX0) addr = (_SHIFTL(texidx,2,8));
else {
    texidx -= GX_DTTMTX0;
    addr = 0x0500 + (_SHIFTL(texidx,2,8));
}
GX_LOAD_XF_REGS(addr,(rows*4));
```

The first branch is the ordinary texture matrix: the *same* `index x 4` arithmetic into the *same*
`0x0000` block. There is no separate texture matrix memory — the GX enum simply starts texture
matrix indices above the last position matrix index. `rows` is 2 for `GX_MTX2x4` and 3 for
`GX_MTX3x4` (`gx.c:2905`), so a texture matrix occupies either two or three rows and the burst is
8 or 12 words.

The conventional partition of the 64 rows, as implied by the GX enum stride of three:

| Rows | Addresses | Contents |
|---|---|---|
| 0–29 | `0x0000`–`0x0077` | Ten position/normal matrix slots (`GX_PNMTX0`..`GX_PNMTX9`), indices 0, 3, 6, … 27 |
| 30–59 | `0x0078`–`0x00EF` | Ten texture matrix slots (`GX_TEXMTX0`..`GX_TEXMTX9`), indices 30, 33, … 57 |
| 60–63 | `0x00F0`–`0x00FF` | The identity matrix slot (`GX_IDENTITY`, index 60) |

**Unverified:** the numeric values of `GX_PNMTX0`, `GX_TEXMTX0` and `GX_IDENTITY` come from
`gx.h`, which was not part of this analysis; the row assignment in the table above is therefore the
*convention*, not a hardware constraint. What **is** certain from `gx.c` is the arithmetic
(`address = index x 4`), the index width (6 bits), the row size (four floats), and that position
and texture matrices share one address space. The stride of three between consecutive slots is
corroborated by the normal-matrix address derivation in §A.2, which is only self-consistent if
position matrix indices step by three.

## A.2 Normal matrix memory (`0x0400`)

`GX_LoadNrmMtxImm()` and `GX_LoadNrmMtxImm3x3()` (`gx.c:2884-2894`):

```c
GX_LOAD_XF_REGS((0x0400|(pnidx*3)),9);
```

* **Base address `0x0400`.**
* **Layout 3x3**: the burst is **9 words**, three rows of three floats. Note this is *not* padded
  to four floats per row — normal matrix memory is packed tighter than position matrix memory.
  `WriteMtxPS3x3from4x3()` (`gx.c:2887`) reads a 3x4 `Mtx` and drops the translation column.
* **The normal matrix index is derived from the position matrix index**, not supplied separately.
  `pnidx` is the *same* `GX_PNMTX*` value passed to `GX_LoadPosMtxImm()`, and the address is
  `0x0400 + 3 * pnidx`.

Expressed two ways, both exact:

```
normal_address = 0x0400 + 3 * position_index
normal_address = 0x0400 + 3 * (position_address / 4)
```

Because a position matrix slot is three rows apart from the next (§A.1), consecutive slot *n* has
`position_index = 3n` and therefore `normal_address = 0x0400 + 9n` — nine words per slot, exactly
the burst size. The `pnidx * 3` expression is what makes the two packings line up.

Ten slots occupy `0x0400`–`0x0459`. With the full 6-bit index range the region extends to
`0x0400 + 3 * 63 = 0x04BD`, which stays clear of the `0x0500` block.

The indexed form confirms the address (`gx.c:2899`):
`((_SHIFTL(mtxidx,16,16))|0x8000|(0x0400|(pnidx*3)))` — count field 8, i.e. 9 words.

## A.3 Dual / post-transform texture matrix memory (`0x0500`)

From the second branch of `GX_LoadTexMtxImm()` quoted in §A.1:

```c
texidx -= GX_DTTMTX0;
addr = 0x0500 + (_SHIFTL(texidx,2,8));
```

* **Base address `0x0500`.**
* Same **rows of four floats** layout as §A.1: address = `0x0500 + index x 4`.
* The index stored in the register is **biased**: the API's `GX_DTTMTX*` constant minus
  `GX_DTTMTX0`. `GX_SetTexCoordGen2()` applies the identical bias before packing the index into the
  post-transform info register (`gx.c:2744-2746`):

  ```c
  postmtx -= GX_DTTMTX0;
  __gx->texCoordGen2[txc] = ((_SHIFTL(normalize,8,1))|(postmtx&0x3f));
  ```

  So the hardware field is 6 bits and **already zero-based against `0x0500`** — an emulator must
  not re-apply the bias.
* The region therefore spans **64 rows, `0x0500`–`0x05FF`**, bounded above by the light memory at
  `0x0600`. At three rows per 3x4 matrix that is room for 20 matrices plus an identity slot, which
  matches the GX enum's 20 `GX_DTTMTX*` constants.

**Unverified:** the numeric value of `GX_DTTMTX0` and of `GX_DTTIDENTITY` (both in `gx.h`). The
bias arithmetic and the 6-bit field width are certain; the absolute enum values are not.

## A.4 Light memory (`0x0600`)

`GX_LoadLightObj()` (`gx.c:4587-4641`):

```c
reg = 0x600|(_SHIFTL(id,4,8));
GX_LOAD_XF_REGS(reg,16);
wgPipe->U32 = 0;
wgPipe->U32 = 0;
wgPipe->U32 = 0;
wgPipe->U32 = lit->col;
wgPipe->F32 = lit->a0;
wgPipe->F32 = lit->a1;
wgPipe->F32 = lit->a2;
wgPipe->F32 = lit->k0;
wgPipe->F32 = lit->k1;
wgPipe->F32 = lit->k2;
wgPipe->F32 = lit->px;
wgPipe->F32 = lit->py;
wgPipe->F32 = lit->pz;
wgPipe->F32 = lit->nx;
wgPipe->F32 = lit->ny;
wgPipe->F32 = lit->nz;
```

* **Base address `0x0600`.**
* **One light's block is 16 words** (`_SHIFTL(id,4,8)` is `id << 4`), so light *i* starts at
  `0x0600 + 16i` and the eight lights occupy `0x0600`–`0x067F`.
* The whole block is always written as a single 16-word burst; libogc has no partial-update path.

Field order within a block:

| Word | Offset | Field | Type |
|---|---|---|---|
| 0 | +0x0 | Reserved | written as 0 |
| 1 | +0x1 | Reserved | written as 0 |
| 2 | +0x2 | Reserved | written as 0 |
| 3 | +0x3 | Light colour | RGBA8, `(r<<24)\|(g<<16)\|(b<<8)\|a` — `gx.c:4584` |
| 4 | +0x4 | `a0` — cosine (angle/spot) attenuation, constant term | f32 |
| 5 | +0x5 | `a1` — cosine attenuation, linear term | f32 |
| 6 | +0x6 | `a2` — cosine attenuation, quadratic term | f32 |
| 7 | +0x7 | `k0` — distance attenuation, constant term | f32 |
| 8 | +0x8 | `k1` — distance attenuation, linear term | f32 |
| 9 | +0x9 | `k2` — distance attenuation, quadratic term | f32 |
| 10 | +0xA | Position X | f32 |
| 11 | +0xB | Position Y | f32 |
| 12 | +0xC | Position Z | f32 |
| 13 | +0xD | Direction X | f32 |
| 14 | +0xE | Direction Y | f32 |
| 15 | +0xF | Direction Z | f32 |

The a/k split is unambiguous in the source: `GX_InitLightSpot()` (`gx.c:4808-4863`) writes only
`a0`/`a1`/`a2` from the spot cutoff cosine, and `GX_InitLightDistAttn()` (`gx.c:4695-4730`) writes
only `k0`/`k1`/`k2` from the reference distance and brightness. `GX_InitLightAttn()`
(`gx.c:4732-4742`) takes them in the order `a0, a1, a2, k0, k1, k2` — the same order as the block.

Two conventions that an emulator must reproduce rather than "fix":

* **The direction is negated on the way in.** `GX_InitLightDir()` (`gx.c:4686-4693`) stores
  `-nx, -ny, -nz`. The words in XF memory point *from* the surface *towards* the light.
* **Specular lights reuse the position and direction fields for something else.**
  `GX_InitSpecularDir()` (`gx.c:4779-4806`) puts the light direction scaled by a very large
  constant into the *position* words and the half-angle vector into the *direction* words
  (`gx.c:4796-4805`). That is how a "directional" specular light is expressed with a positional
  light block; the hardware does not have a separate mode bit for it.

The indexed form (`gx.c:4679-4683`) writes `0xf600|(id<<4)`: count field 15, i.e. 16 words at
`0x0600 + 16i` — an independent confirmation of both base and stride.

**Unverified:** the meaning of words 0–2. libogc always writes zero and never reads them back.

---

# B. XF registers (`0x1000` and above)

## B.1 Status and diagnostic registers (`0x1000`–`0x1007`)

Only `0x1000` is written by libogc, once, in `__GX_InitRevBits()` (`gx.c:250`):

```c
GX_LOAD_XF_REG(0x1000,0x3f);
```

| Address | Name | Payload |
|---|---|---|
| `0x1000` | Error | `0x3f` — six bits set, consistent with an error-mask or write-one-to-clear register with six error sources. **Unverified:** the meaning of the individual bits. |
| `0x1001` | Diagnostics | **Unverified:** never written by libogc; not present in this tree's trimmed `XFMemory.h`. |
| `0x1002` | State 0 | **Unverified:** never written. |
| `0x1003` | State 1 | **Unverified:** never written. |
| `0x1004` | Clock | **Unverified:** never written. |
| `0x1007` | *unassigned* | **Unverified:** never written; no evidence it exists. |

The *names* in this table are the customary GameCube ones and are given so a decoder's address table
is complete. They cannot be confirmed from either source available here — treat every one of
`0x1001`–`0x1004` and `0x1007` as an opaque word, and log rather than act on any write to them.

## B.2 Clip disable (`0x1005`)

`GX_SetClipMode()` (`gx.c:3952-3955`):

```c
GX_LOAD_XF_REG(0x1005,(mode&1));
```

| Bits | Field |
|---|---|
| 0 | Clip disable: **0 = clipping enabled, 1 = clipping disabled** |
| 31:1 | **Unverified:** not written. |

The polarity is fixed by `GX_Init()`, which calls `GX_SetClipMode(GX_CLIP_ENABLE)` (`gx.c:646`) —
and by `clipping.cpp`, which drives the register with `step - 7`, `step - 9`, `step - 11`,
`step - 13` to toggle between a clipped and an unclipped case, i.e. 0 then 1
(`clipping.cpp:160-192`). That test exercises **depth** clipping specifically, so bit 0 at minimum
disables near/far plane rejection.

**Unverified:** whether the hardware field is wider than one bit (the customary layout splits
clipping disable from a separate "disable trivial reject" bit). libogc masks with `&1` and never
sets more.

## B.3 Performance metric select (`0x1006`)

Written by `GX_SetGPMetric()` (`gx.c:4873-4921`), `GX_InitXfRasMetric()` (`gx.c:5086`) and
`GX_Init()` (`gx.c:1230`).

| Bits | Field |
|---|---|
| 23:0 | Metric selector. **Unverified:** field layout. |

libogc only ever writes opaque literal constants, one per metric:

| Metric | Value |
|---|---|
| Disable / clear | `0x00000000` (`gx.c:1230`, `gx.c:4873`) |
| Clocks | `0x00000273` |
| Vertices | `0x0000014a` |
| Clipped vertices | `0x0000016b` |
| Clip clocks | `0x00000084` |
| XF wait-in | `0x000000c6` |
| XF wait-out | `0x00000210` |
| XF transform clocks | `0x00000252` |
| XF lighting clocks | `0x00000231` |
| XF bottom clocks | `0x000001ad` |
| XF register-load clocks | `0x000001ce` |
| XF register-read clocks | `0x00000021` |
| Clip ratio | `0x00000153` |
| Combined XF/RAS metric | `0x00031000` (`gx.c:5086`) |

The results are read back through CP registers, not through the XF (`GX_ReadXfRasMetric()`,
`gx.c:5089-5095`), so an emulator that does not implement performance counters can safely ignore
every write to `0x1006`. Listed so a command-stream decoder does not mis-parse them.

## B.4 Vertex specs (`0x1008`)

`__GX_XfVtxSpecs()` (`gx.c:751-776`):

```c
xfvtxspecs = (_SHIFTL(texs,4,4))|(_SHIFTL(nrms,2,2))|(cols&0x3);
GX_LOAD_XF_REG(0x1008,xfvtxspecs);
```

| Bits | Field | Evidence |
|---|---|---|
| 1:0 | Number of colour channels present in the vertex, 0..2 | counted from CP vertex descriptor bits: `if(__gx->vcdLo&0x6000) cols++; if(__gx->vcdLo&0x18000) cols++;` — `gx.c:757-758` |
| 3:2 | Normal specification: 0 = none, 1 = normal only, 2 = normal + binormal + tangent | `if(__gx->vcdNrms==1) nrms = 1; else if(__gx->vcdNrms==2) nrms = 2;` — `gx.c:760-762` |
| 7:4 | Number of texture coordinates present in the vertex, 0..8 | one increment per populated 2-bit pair of `__gx->vcdHi` — `gx.c:765-772` |
| 31:8 | **Unverified:** not written. |

This register mirrors the CP vertex descriptor into the XF so the transform unit knows how many
attributes to expect. libogc rewrites it from `__GX_SetVCD()` (`gx.c:823-828`) every time the
descriptor changes; the two must be kept consistent.

Note that value 2 in bits 3:2 means **three** vectors are supplied (normal, binormal, tangent) —
that is how the emboss texgen source rows 3 and 4 in §B.13 get their input.

## B.5 Number of colour channels (`0x1009`)

`__GX_SetChanCntrl()` (`gx.c:1010`):

```c
if(__gx->dirtyState&0x01000000) GX_LOAD_XF_REG(0x1009,(_SHIFTR(__gx->genMode,4,3)));
```

| Bits | Field |
|---|---|
| 2:0 | Number of colour channels to compute, 0..2 |
| 31:3 | **Unverified:** not written. |

The value is a copy of BP `genMode` bits 6:4, written by `GX_SetNumChans()` (`gx.c:3984`). The two
registers must agree: the BP number drives rasterisation, the XF number drives lighting.
`lighting.cpp:22-23` pokes this register directly with the literal 1.

## B.6 Colour and alpha channel control (`0x100E`–`0x1011`)

Four registers, dispatched by `__GX_SetChanCntrl()` from a base of `0x100e` (`gx.c:1013-1016`):

| Address | Channel |
|---|---|
| `0x100E` | Colour channel 0 |
| `0x100F` | Colour channel 1 |
| `0x1010` | Alpha channel 0 |
| `0x1011` | Alpha channel 1 |

`GX_SetChanCtrl()` builds all four from one expression (`gx.c:2196`):

```c
u32 val = (matsrc&1)|(_SHIFTL(enable,1,1))|(_SHIFTL(litmask,2,4))|(_SHIFTL(ambsrc,6,1))
        |(_SHIFTL(difffn,7,2))|(_SHIFTL(((GX_AF_NONE-attn_fn)>0),9,1))
        |(_SHIFTL((attn_fn>0),10,1))|(_SHIFTL((_SHIFTR(litmask,4,4)),11,4));
```

This is the "lit channel" word — the most important register in the XF. Every bit:

| Bits | Field | Meaning |
|---|---|---|
| 0 | Material source | 0 = the material colour register (`0x100C`/`0x100D`), 1 = the vertex colour |
| 1 | Lighting enable | 0 = pass the material colour straight through, 1 = run the lighting equation |
| 5:2 | **Light mask, lights 0–3** | one bit per light; bit 2 = light 0 … bit 5 = light 3 |
| 6 | Ambient source | 0 = the ambient colour register (`0x100A`/`0x100B`), 1 = the vertex colour |
| 8:7 | Diffuse function | see below |
| 10:9 | Attenuation function | see below |
| 14:11 | **Light mask, lights 4–7** | bit 11 = light 4 … bit 14 = light 7 |
| 31:15 | **Unverified:** not written. |

**Cross-checked against Dolphin.** This is the one register family the trimmed `XFMemory.h` could
confirm: it declares fields at exactly bits 0, 1, 2 (4 wide), 6, 7 (2 wide), 9 (2 wide) and 11
(4 wide), and it reconstructs the full eight-light mask as the low nibble OR the high nibble shifted
left by four — the same split libogc writes. It also records the top 17 bits as unused, which
matches libogc never writing above bit 14.

**The split light mask is the trap.** The eight-light selection is *not* a contiguous byte. libogc
takes an 8-bit API mask and writes its low nibble at bits 5:2 and its high nibble at bits 14:11:

```c
(_SHIFTL(litmask,2,4)) ... (_SHIFTL((_SHIFTR(litmask,4,4)),11,4))
```

An emulator must reassemble `mask = bits[5:2] | (bits[14:11] << 4)`, and — per Dolphin's
reconstruction — must treat the mask as **zero when bit 1 (lighting enable) is clear**, not merely
ignore it.

### Diffuse function (bits 8:7)

| Value | Meaning |
|---|---|
| 0 | None — the diffuse term is 1.0 (no N·L factor) |
| 1 | Signed — use N·L as-is, including negative values |
| 2 | Clamped — use max(N·L, 0) |
| 3 | **Unverified:** not produced by libogc. |

These are the `GX_DF_NONE` / `GX_DF_SIGN` / `GX_DF_CLAMP` enum values passed through unmodified.
**Unverified:** the numeric enum values live in `gx.h`, which was not read; the *bit position* 8:7
is certain, and the ordering above is the conventional one. One behaviour is provable from `gx.c`:
`u32 difffn = (attn_fn==GX_AF_SPEC)?GX_DF_NONE:diff_fn;` (`gx.c:2195`) — **a specular channel always
forces the diffuse function to "none"**, because in specular mode the N·L slot is used by the
specular computation.

### Attenuation function (bits 10:9)

libogc does not pass an enum through here; it *computes* the two bits from the API value
(`gx.c:2196`):

* bit 9 = `(GX_AF_NONE - attn_fn) > 0` — set for the two real attenuation modes
* bit 10 = `attn_fn > 0` — set for everything except specular

With the GX enum ordering `GX_AF_SPEC` < `GX_AF_SPOT` < `GX_AF_NONE`, that yields:

| Value (bits 10:9) | libogc input | Meaning |
|---|---|---|
| 0 | — | No attenuation. Not produced by `GX_SetChanCtrl()`. |
| 1 | `GX_AF_SPEC` | Specular: the light's position words hold a direction, the direction words hold the half-angle vector, and the a/k coefficients shape the specular falloff |
| 2 | `GX_AF_NONE` | Directional: no attenuation applied |
| 3 | `GX_AF_SPOT` | Spot: the a coefficients give the cosine/angle falloff and the k coefficients the distance falloff |

Read the two bits separately and the encoding makes sense: **bit 9 means "an attenuation
computation runs", bit 10 means "this is not a specular light"**. Values 0 and 2 both mean no
attenuation.

Dolphin confirms only the field's position and width (2 bits at 9). The value ordering above is
derived from libogc's arithmetic plus the `GX_AF_*` enum ordering; **Unverified:** the numeric
`GX_AF_*` values themselves (`gx.h`).

### Register pairing

`GX_SetChanCtrl()` writes the *same* word to two shadow slots (`gx.c:2202-2208`): a call for
`GX_COLOR0A0` fills both the colour-0 and alpha-0 registers, a call for `GX_COLOR1A1` both of the
channel-1 registers. `lighting.cpp:30-33` does the same thing by hand — the identical word to
`0x100e` and `0x1010`. Colour and alpha are genuinely independent registers; they just usually
carry the same configuration.

## B.7 Ambient and material colours (`0x100A`–`0x100D`)

Written by `__GX_SetChanColor()` (`gx.c:994-1004`):

| Address | Contents |
|---|---|
| `0x100A` | Channel 0 ambient colour |
| `0x100B` | Channel 1 ambient colour |
| `0x100C` | Channel 0 material colour |
| `0x100D` | Channel 1 material colour |

Both `GX_SetChanAmbColor()` (`gx.c:2213`) and `GX_SetChanMatColor()` (`gx.c:2249`) pack identically:

```c
u32 val = (_SHIFTL(color.r,24,8))|(_SHIFTL(color.g,16,8))|(_SHIFTL(color.b,8,8))|0x00;
```

| Bits | Field |
|---|---|
| 7:0 | Alpha |
| 15:8 | Blue |
| 23:16 | Green |
| 31:24 | Red |

i.e. **RGBA8 with red in the most significant byte** — a straight big-endian RGBA byte order, and
notably *not* the BGR / red-with-alpha orderings used by several BP colour registers.

These are 32-bit payloads; unlike BP registers there is no register number stealing the top byte.

The colour and alpha halves are independently settable: the `GX_ALPHA0` / `GX_ALPHA1` cases of both
functions preserve the upper 24 bits and replace only the low byte (`gx.c:2225`, `gx.c:2261`), while
the `GX_COLOR0` / `GX_COLOR1` cases preserve the low byte and replace the upper 24.
`lighting.cpp:68-72` writes `(colour << 24) | 255` to `0x100a` and `0x100c` — red only, alpha 255 —
which matches this layout exactly.

## B.8 Dual-texture-transform enable (`0x1012`)

Written once, in `__GX_InitRevBits()` (`gx.c:251`):

```c
GX_LOAD_XF_REG(0x1012,0x01);
```

| Bits | Field |
|---|---|
| 0 | Dual-texture-transform enable: 1 = the post-transform matrices of §A.3 and the per-texgen post-transform info of §B.14 are applied after the ordinary texture matrix |
| 31:1 | **Unverified:** not written. |

libogc enables it unconditionally at init and never turns it off, which is why
`GX_SetTexCoordGen()` can always default the post-transform matrix to the identity
(`gx.c:2655`) rather than having to gate the feature. An emulator that ignores this bit and always
applies the post-transform stage will still be correct for libogc-built titles, but not for a title
that clears it.

**Unverified:** whether bit 0 gates the post-transform *matrix* only or also the normalise step of
§B.14.

## B.9 Matrix index A and B (`0x1018`, `0x1019`)

`__GX_SetMatrixIndex()` (`gx.c:778-787`):

```c
if(mtx<5) {
    GX_LOAD_CP_REG(0x30,__gx->mtxIdxLo);
    GX_LOAD_XF_REG(0x1018,__gx->mtxIdxLo);
} else {
    GX_LOAD_CP_REG(0x40,__gx->mtxIdxHi);
    GX_LOAD_XF_REG(0x1019,__gx->mtxIdxHi);
}
```

The same two words are also sent to CP registers `0x30` and `0x40`. The XF copy is what the
transform unit uses; the CP copy is what the vertex loader uses when a vertex supplies its own
matrix index. **They must be kept identical.**

Nine 6-bit indices are packed across the two registers: one position/normal index and eight texture
matrix indices. The packing comes from `GX_SetCurrentMtx()` (`gx.c:2933`) and the switch at the end
of `GX_SetTexCoordGen2()` (`gx.c:2748-2773`).

**`0x1018` — matrix index A:**

| Bits | Field | Evidence |
|---|---|---|
| 5:0 | Position / normal matrix index | `(__gx->mtxIdxLo&~0x3f)\|(mtx&0x3f)` — `gx.c:2933` |
| 11:6 | Texture coordinate 0 matrix index | `(_SHIFTL(mtxsrc,6,6))` — `gx.c:2750` |
| 17:12 | Texture coordinate 1 matrix index | `(_SHIFTL(mtxsrc,12,6))` — `gx.c:2753` |
| 23:18 | Texture coordinate 2 matrix index | `(_SHIFTL(mtxsrc,18,6))` — `gx.c:2756` |
| 29:24 | Texture coordinate 3 matrix index | `(_SHIFTL(mtxsrc,24,6))` — `gx.c:2759` |
| 31:30 | **Unverified:** not written. |

**`0x1019` — matrix index B:**

| Bits | Field | Evidence |
|---|---|---|
| 5:0 | Texture coordinate 4 matrix index | `(mtxsrc&0x3f)` — `gx.c:2762` |
| 11:6 | Texture coordinate 5 matrix index | `(_SHIFTL(mtxsrc,6,6))` — `gx.c:2765` |
| 17:12 | Texture coordinate 6 matrix index | `(_SHIFTL(mtxsrc,12,6))` — `gx.c:2768` |
| 23:18 | Texture coordinate 7 matrix index | `(_SHIFTL(mtxsrc,18,6))` — `gx.c:2771` |
| 31:24 | **Unverified:** not written. |

So register A carries **the position index plus texture coordinates 0–3** (five fields, 30 bits) and
register B carries **texture coordinates 4–7** (four fields, 24 bits). The position index is the odd
one out — it is the only field that is not a texgen, and it is the reason A holds five indices and B
only four.

Every index is a **row number in the `0x0000` block** and is used exactly as described in §A.1: XF
address = index x 4, and for the position index the normal matrix is found at `0x0400 + 3 x index`
(§A.2).

The `if(mtx<5)` dispatch selects which of the two registers to emit; its threshold of 5 matches the
five index fields packed into register A. libogc's only caller invokes it twice, with 0 and then 5
(`gx.c:1052-1053`), so in practice both registers are always rewritten together whenever any index
changes.

## B.10 Viewport (`0x101A`–`0x101F`)

`GX_SetViewportJitter()` (`gx.c:1767-1791`) writes all six as one burst:

```c
static const f32 Xfactor = 0.5f;
static const f32 Yfactor = 342.0f;
static const f32 Zfactor = 16777215.0f;

if(!field) yOrig -= Xfactor;

x0 = wd*Xfactor;
y0 = (-ht)*Xfactor;
x1 = (xOrig+(wd*Xfactor))+Yfactor;
y1 = (yOrig+(ht*Xfactor))+Yfactor;
n = Zfactor*nearZ;
f = Zfactor*farZ;
z = f-n;

GX_LOAD_XF_REGS(0x101a,6);
wgPipe->F32 = x0;
wgPipe->F32 = y0;
wgPipe->F32 = z;
wgPipe->F32 = x1;
wgPipe->F32 = y1;
wgPipe->F32 = f;
```

| Address | Word | Value written | Meaning |
|---|---|---|---|
| `0x101A` | 0 | `width * 0.5` | X scale — half the viewport width, because clip space is [-1, +1] |
| `0x101B` | 1 | `-height * 0.5` | Y scale — **negative**, flipping clip-space +Y (up) to screen +Y (down) |
| `0x101C` | 2 | `(far - near) * 16777215` | Z scale — the depth range mapped onto the full 24-bit Z buffer |
| `0x101D` | 3 | `xOrigin + width*0.5 + 342` | X offset — viewport centre, **biased by +342** |
| `0x101E` | 4 | `yOrigin + height*0.5 + 342` | Y offset — viewport centre, **biased by +342** |
| `0x101F` | 5 | `far * 16777215` | Z offset — the far plane's 24-bit depth value |

All six are IEEE-754 single-precision floats.

**The 342 origin.** The constant is `0x156` in decimal, the same bias the BP scissor registers use
(`gc-bp-registers.md` §3.1). The GameCube's internal screen coordinate space puts the top-left of
the EFB at (342, 342); the viewport offsets, the scissor rectangle and the fog range centre all
carry the bias, so they cancel out and an emulator can subtract 342 from all of them consistently.
It is *not* a signed-coordinate trick specific to the viewport.

**Z convention.** Note that the scale is `(far - near) * 16777215` while the offset is
`far * 16777215`, not `near * 16777215` — the depth offset is anchored at the **far** plane, which
is the opposite of the X and Y words (whose offsets are viewport centres). Both words are
pre-multiplied by `16777215` = 2^24 − 1, so they are already in 24-bit EFB depth units and an
emulator should not scale them again.

**Unverified:** the exact way the hardware combines words 2 and 5 with the post-divide Z. The
anchoring at `far` means it cannot be the naive `near + (far-near)*z`; the two consistent readings
are `z_screen = far*M + z_ndc*(far-near)*M` with `z_ndc` in [-1, 0], or the equivalent
`z_screen = far*M - z_clip*(far-near)*M` with `z_clip` in [0, 1]. `gx.c` transmits the two numbers
but never states the combining rule, and this tree's trimmed `XFMemory.h` does not contain the
viewport declaration.

**The `field` argument** (`gx.c:1774`) subtracts half a scanline from the Y origin for interlaced
jitter. `GX_SetViewport()` is simply `GX_SetViewportJitter(..., 1)` (`gx.c:1795`), which skips the
adjustment. There is no hardware field for this — it is folded into word 4.

Cross-checked against Dolphin's test harness: `CGX_SetViewport()` (`cgx.cpp:86-95`) writes the same
six expressions in the same order with the same 342 and 16777215 constants, independently derived.

## B.11 Projection (`0x1020`–`0x1026`)

`GX_LoadProjectionMtx()` (`gx.c:1798-1831`) writes **seven** words: six floats and one integer flag.

```c
((u32*)((void*)tmp))[6] = (u32)type;
tmp[0] = mt[0][0];
tmp[2] = mt[1][1];
tmp[4] = mt[2][2];
tmp[5] = mt[2][3];

switch(type) {
    case GX_PERSPECTIVE:
        tmp[1] = mt[0][2];
        tmp[3] = mt[1][2];
        break;
    case GX_ORTHOGRAPHIC:
        tmp[1] = mt[0][3];
        tmp[3] = mt[1][3];
        break;
    default:
        tmp[1] = 0.0;
        tmp[3] = 0.0;
        break;
}

GX_LOAD_XF_REGS(0x1020,7);
```

**Word 6 (`0x1026`) is the mode flag**, and it is an integer, not a float — the cast
`((u32*)((void*)tmp))[6] = (u32)type;` writes the raw enum value into the float array's seventh
slot.

| Bits of `0x1026` | Field |
|---|---|
| 0 | Projection type: **0 = perspective, 1 = orthographic** |
| 31:1 | **Unverified:** not written beyond the enum value. |

The six numbers before it are **not a matrix** — they are the only six entries of a 4x4 projection
matrix that the hardware needs, and **their meaning changes with the flag**:

| Address | Word | Perspective (flag = 0) | Orthographic (flag = 1) |
|---|---|---|---|
| `0x1020` | 0 | `m[0][0]` — X scale | `m[0][0]` — X scale |
| `0x1021` | 1 | `m[0][2]` — X shear against Z (off-centre frustum) | `m[0][3]` — X translation |
| `0x1022` | 2 | `m[1][1]` — Y scale | `m[1][1]` — Y scale |
| `0x1023` | 3 | `m[1][2]` — Y shear against Z | `m[1][3]` — Y translation |
| `0x1024` | 4 | `m[2][2]` — Z scale | `m[2][2]` — Z scale |
| `0x1025` | 5 | `m[2][3]` — Z translation | `m[2][3]` — Z translation |
| `0x1026` | 6 | `0` | `1` |

The difference is entirely in words 1 and 3. In a perspective matrix the X and Y offsets live in
column 2 (they are scaled by Z, giving an off-axis frustum); in an orthographic matrix they live in
column 3 (a plain translation). Words 0, 2, 4 and 5 are taken from the same four positions either
way.

The fourth row is not transmitted at all. The hardware supplies it implicitly: `(0,0,-1,0)` for
perspective (so `w_clip = -z_eye`) and `(0,0,0,1)` for orthographic (so `w_clip = 1`). **Unverified:**
the exact implicit fourth row — it is not written anywhere in `gx.c`, and is inferred from the fact
that six numbers cannot otherwise describe a usable projection. The *selection* of which six, and
the flag that distinguishes them, are certain.

For any `type` other than the two defined values libogc zeroes words 1 and 3 (`gx.c:1817-1820`) but
still transmits the unrecognised value in word 6.

Cross-checked against Dolphin's test harness: the commented-out reference implementations in
`cgx.cpp:108-129` list exactly these six matrix entries in exactly this order for the two modes,
with the flag 0 and 1 respectively.

## B.12 Number of texture generators (`0x103F`)

`__GX_SetTexCoordGen()` (`gx.c:978`):

```c
if(__gx->dirtyState&0x02000000) GX_LOAD_XF_REG(0x103f,(__gx->genMode&0xf));
```

| Bits | Field |
|---|---|
| 3:0 | Number of texture coordinates to generate, 0..8 |
| 31:4 | **Unverified:** not written. |

A copy of BP `genMode` bits 3:0, set by `GX_SetNumTexGens()` (`gx.c:2939`). As with the colour
channel count (§B.5), the BP and XF copies must agree.

## B.13 Texture coordinate generation info (`0x1040`–`0x1047`)

One register per texgen; `__GX_SetTexCoordGen()` walks a dirty mask from a base of `0x1040`
(`gx.c:981-990`), so texgen *n* is at `0x1040 + n`.

Built by `GX_SetTexCoordGen2()` (`gx.c:2719-2745`):

```c
if((tgen_typ==GX_TG_MTX3x4 || tgen_typ==GX_TG_MTX2x4))
{
    if(tgen_typ==GX_TG_MTX3x4) texcoords = 0x02;
    texcoords |= (_SHIFTL(stq,2,1));
    texcoords |= (_SHIFTL(vtxrow,7,5));
} else if((tgen_typ>=GX_TG_BUMP0 && tgen_typ<=GX_TG_BUMP7))
{
    tgen_src -= GX_TG_TEXCOORD0;
    tgen_typ -= GX_TG_BUMP0;
    texcoords = 0x10;
    texcoords |= (_SHIFTL(stq,2,1));
    texcoords |= (_SHIFTL(vtxrow,7,5));
    texcoords |= (_SHIFTL(tgen_src,12,3));
    texcoords |= (_SHIFTL(tgen_typ,15,3));
} else if(tgen_typ==GX_TG_SRTG) {
    if(tgen_src==GX_TG_COLOR0) texcoords = 0x20;
    else if(tgen_src==GX_TG_COLOR1) texcoords = 0x30;
    texcoords |= (_SHIFTL(stq,2,1));
    texcoords |= (_SHIFTL(2,7,5));
}
```

| Bits | Field | Evidence |
|---|---|---|
| 0 | **Unverified:** never written by libogc. |  |
| 1 | Projection: 0 = 2x4 matrix, output (s, t); 1 = 3x4 matrix, output (s, t, q) with a perspective divide | `texcoords = 0x02` for `GX_TG_MTX3x4` — `gx.c:2723` |
| 2 | Input form: 0 = the source row is used as (A, B, 1, 1); 1 = as (A, B, C, 1) | `(_SHIFTL(stq,2,1))` — `gx.c:2725`. `stq` is set to 1 only for the position, normal, binormal and tangent sources (`gx.c:2668-2683`) — the sources that genuinely have three components |
| 3 | **Unverified:** never written. |  |
| 6:4 | Texgen type | the literals `0x00`, `0x10`, `0x20`, `0x30` are `0`, `1`, `2`, `3` at bit 4 — `gx.c:2723, 2732, 2738-2739` |
| 11:7 | Source row — which vertex attribute feeds the generator | `(_SHIFTL(vtxrow,7,5))` — `gx.c:2726, 2734, 2741` |
| 14:12 | Emboss source: which *texture coordinate* supplies the base ST (emboss type only) | `(_SHIFTL(tgen_src,12,3))` — `gx.c:2735` |
| 17:15 | Emboss light: which *light* supplies the direction (emboss type only) | `(_SHIFTL(tgen_typ,15,3))` — `gx.c:2736` |
| 31:18 | **Unverified:** not written. |  |

### Texgen type (bits 6:4)

| Value | libogc source | Meaning |
|---|---|---|
| 0 | `GX_TG_MTX2x4` / `GX_TG_MTX3x4` | Regular: multiply the source row by the texture matrix named in §B.9. Bit 1 picks 2x4 vs 3x4. |
| 1 | `GX_TG_BUMP0`..`GX_TG_BUMP7` | Emboss / bump: perturb an existing texture coordinate using a light direction. Bits 14:12 and 17:15 are meaningful only here. |
| 2 | `GX_TG_SRTG` with `GX_TG_COLOR0` | Colour: use vertex colour channel 0 as the texture coordinate |
| 3 | `GX_TG_SRTG` with `GX_TG_COLOR1` | Colour: use vertex colour channel 1 |

Types 2 and 3 are the same operation with a different channel; libogc encodes the channel in the
type field rather than in the source row, and forces the source row to the literal 2 (the colour
row) for both (`gx.c:2741`).

### Source row (bits 11:7)

Assigned in the switch at `gx.c:2667-2717`:

| Value | Source |
|---|---|
| 0 | Position |
| 1 | Normal |
| 2 | Colour (used for both colour channels; the channel comes from the type field) |
| 3 | Binormal |
| 4 | Tangent |
| 5 | Texture coordinate 0 |
| 6 | Texture coordinate 1 |
| 7 | Texture coordinate 2 |
| 8 | Texture coordinate 3 |
| 9 | Texture coordinate 4 |
| 10 | Texture coordinate 5 |
| 11 | Texture coordinate 6 |
| 12 | Texture coordinate 7 |

The default for an unrecognised source is 5, texture coordinate 0 (`gx.c:2714-2716`). Rows 3 and 4
are only populated when the vertex specs register (§B.4) declares normals *and* binormals/tangents.

### Emboss fields (bits 14:12, 17:15)

Both are biased before packing: the source is `tgen_src - GX_TG_TEXCOORD0` and the light is
`tgen_typ - GX_TG_BUMP0` (`gx.c:2729-2730`), so both fields are zero-based 3-bit values — texture
coordinate 0..7 and light 0..7 respectively. They are left at zero for every non-emboss type
because the three `texcoords = ...` assignments overwrite the whole word.

## B.14 Post-transform texture info (`0x1050`–`0x1057`)

Emitted alongside the previous register with a fixed `+0x10` offset (`gx.c:985-986`):

```c
GX_LOAD_XF_REG(texcoord,__gx->texCoordGen[i]);
GX_LOAD_XF_REG((texcoord+0x10),__gx->texCoordGen2[i]);
```

so texgen *n* is at `0x1050 + n`, and the two registers for a texgen are always written as a pair.

Built by `GX_SetTexCoordGen2()` (`gx.c:2744-2746`):

```c
postmtx -= GX_DTTMTX0;
__gx->texCoordGen2[txc] = ((_SHIFTL(normalize,8,1))|(postmtx&0x3f));
```

| Bits | Field |
|---|---|
| 5:0 | Post-transform matrix index — a row number in the `0x0500` block (§A.3), already biased to be zero-based |
| 7:6 | **Unverified:** not written. |
| 8 | Normalise: 1 = normalise the transformed coordinate to unit length *before* applying the post-transform matrix |
| 31:9 | **Unverified:** not written. |

The normalise bit is what makes reflection/environment mapping work: the ordinary texture matrix
produces a direction vector, the normalise step turns it into a unit vector, and the post-transform
matrix maps it into [0, 1] texture space.

The whole stage is gated by the dual-texture-transform enable (§B.8, register `0x1012`).
`GX_SetTexCoordGen()` — the one-line wrapper at `gx.c:2653-2656` — supplies `GX_FALSE` and
`GX_DTTIDENTITY`, so a title that never calls `GX_SetTexCoordGen2()` leaves every post-transform
matrix pointing at the identity slot with normalisation off.

---

## C. Summary of what could not be determined

The following are recorded as **Unverified:** above and should be treated as unknown by an emulator
until hardware-tested:

1. **Registers `0x1001`–`0x1004` and `0x1007`** (diagnostics, state 0, state 1, clock). libogc never
   writes them; this tree's `XFMemory.h` is trimmed to a single union and does not list them. Only
   their conventional names are given, and even those are uncorroborated.
2. **Error register (`0x1000`) bit meanings.** Only the single constant `0x3f` is ever written.
3. **Clip disable (`0x1005`) width.** libogc masks the value with `&1`. Whether a second bit exists
   (the customary split between "disable clipping" and "disable trivial reject") could not be shown.
4. **Performance metric select (`0x1006`) field layout.** libogc writes only opaque literals, one per
   metric; the fourteen values are tabulated but not decoded.
5. **Words 0–2 of every light block.** Always written as zero, never read back.
6. **Diffuse function enum values (bits 8:7 of `0x100E`–`0x1011`).** The bit position and width are
   confirmed by both sources; the NONE/SIGN/CLAMP numbering comes from `gx.h`, which was not read.
   Value 3 is never produced.
7. **Attenuation function enum values (bits 10:9).** Same situation: Dolphin confirms a 2-bit field
   at bit 9, and libogc's arithmetic gives the mapping, but the `GX_AF_*` numeric values are from
   `gx.h`. Value 0 is never produced by `GX_SetChanCtrl()`.
8. **Bits 31:15 of the lit-channel registers.** Neither source writes them; Dolphin's header records
   the top 17 bits as unused, which is consistent but is an absence of evidence.
9. **Dual-texture-transform (`0x1012`) bits above 0**, and whether bit 0 gates the normalise step of
   §B.14 as well as the post-transform matrix.
10. **Texgen info (`0x1040`+) bits 0 and 3.** Never written by libogc; both sit inside the
    low nibble between confirmed fields, so they are real bits of something.
11. **The implicit fourth row of the projection matrix.** Six numbers plus a mode flag are
    transmitted; the hardware's implied `w` row is inferred from the mathematics, not read from
    either source.
12. **The exact numeric values of the GX matrix-slot enums** (`GX_PNMTX0`, `GX_TEXMTX0`,
    `GX_IDENTITY`, `GX_DTTMTX0`, `GX_DTTIDENTITY`). All the *addressing arithmetic* is proven
    (`address = index x 4`, `normal = 0x0400 + 3 x index`, post-transform is pre-biased against
    `0x0500`); only the absolute enum constants, and hence the exact row at which texture matrices
    begin, rest on the GX convention rather than on `gx.c`.
13. **Every XF address libogc never touches**: `0x0100`–`0x03FF`, `0x0680`–`0x0FFF`,
    `0x1013`–`0x1017`, `0x1027`–`0x103E`, `0x1048`–`0x104F`.
14. **The viewport's depth combining rule.** The two depth words are certain
    (`(far-near) * 16777215` and `far * 16777215`, in that order and at those addresses); how the
    hardware applies them to the post-divide Z, and therefore the sign convention of the pipeline's
    Z, is inferred rather than read.

One structural caution that is *not* unverified, and that an emulator must get right: **XF writes
carry an explicit word count in bits 31:16 of the header.** Unlike a BP write, an XF write is not a
fixed-size packet, and the viewport and projection are only ever written as multi-word bursts.
