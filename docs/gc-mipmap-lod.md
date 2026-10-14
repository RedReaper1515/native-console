# GameCube Flipper: Mipmapping and Level-of-Detail Selection

## Provenance

This is the third texture-side document in the set, after
[`gc-texture-formats.md`](gc-texture-formats.md) (what a texel is and where it lives) and
[`gc-bp-registers.md`](gc-bp-registers.md) (the register file). It covers the part of the texture
unit those two deliberately left open: **which** level a pixel reads, and **how** two levels and
eight texels become one colour. It does not restate the format table, the tile geometry, the
within-level address arithmetic or the wrap modes — §1, §2, §5.1 and §6.2 of the formats document
own those, and everything here builds on them.

Three classes of source, kept strictly apart:

| Source | Licence | Status here |
|---|---|---|
| devkitPro/**libogc** `gx.h` (local, `D:\devkitPro\libogc\include\ogc\gx.h`, 5328 lines) and `gx.c` (upstream `master`) | zlib | **Primary, quotable.** Every register bit position and field unit below is read out of a quoted `gx.c` expression or a `gx.h` doxygen statement. |
| **Dolphin** — `Source/Core/VideoBackends/Software/{Rasterizer,TextureSampler,TransformUnit,Tev}.cpp`, `Source/Core/VideoCommon/{TextureInfo,RenderState,PixelShaderGen}.cpp`, and the trimmed `BPMemory.h` in `roms/gc/hwtests/gxtest/` | GPL-2.0 | **Behaviour reference only.** Read, described in this document's own words, never copied. A claim sourced this way says "Dolphin behaves as if…", which is a statement about behaviour, not a reuse of expression. Nothing from Dolphin has entered this repository. |
| Hardware | — | Nothing here was measured on hardware. |

Because libogc is a *submission* library it settles the register side completely and the algorithm
side not at all. `gx.c` computes a LOD bias in 1/32 steps and stuffs it into bits 16:9; it has
nothing whatever to say about what the GPU then does with it. So the split through this document
is sharp, and it is worth stating up front:

* **§1 (registers), §2 (memory layout), §4.1 (the filter-field decode)** rest on quoted libogc and
  are safe to implement as written.
* **§3 (the LOD formula), §4.2–4.3 (the mag/min rule), §5 (precision)** rest on Dolphin's
  behaviour, cross-checked between its two independent implementations — the software rasteriser
  and the "manual texture sampling" shader the hardware backends generate — which is worth
  something precisely because they were written years apart and disagree in two places. Both
  disagreements are recorded as **Unverified:** below rather than silently resolved.

A previous document in this set shipped a byte order backwards and it cost real time. The rule
that came out of that: prefer a named source to an assertion, and mark the gap rather than fill it.
§7 collects every **Unverified:** item in one table and §8 says what a guest program would have to
draw to close each one.

Line numbers for `gx.c` refer to upstream `master`; the sibling documents were written against a
local 5199-line copy and their line numbers differ by roughly 1150 in the texture region. Where a
sibling already quotes an expression its number is given in that document's numbering and marked.

---

## 1. The registers

Eight texture units. Units 0–3 use the `0x80`–`0x9F` block and units 4–7 the `0xA0`–`0xBB` block,
with identical layouts; the dispatch tables are quoted in the sibling document §8. Five registers
carry LOD state, and one of them is not a texture register at all.

### 1.1 setmode0 (`0x80`–`0x83`, `0xA0`–`0xA3`) — the filter and LOD control word

The sibling document §8.1 gives this register's full layout. Repeated here with the LOD fields
expanded, because this is where nearly all of the machinery lives. Each row's evidence is the
`gx.c` expression that writes it, all of them from `GX_InitTexObjLOD()` (`gx.c:4455-4476`) or its
single-field siblings (`gx.c:4486-4539`).

| Bits | Field | Units / encoding | Evidence |
|---|---|---|---|
| 1:0 | Wrap S | 0 clamp, 1 repeat, 2 mirror | `(wrap_s&3)` |
| 3:2 | Wrap T | same | `_SHIFTL(wrap_t,2,2)` |
| **4** | **Magnification filter** | 0 = point, 1 = linear | `_SHIFTL((magfilt==GX_LINEAR?1:0),4,1)` |
| **6:5** | **Mip mode** | 0 = none, 1 = point, 2 = linear | see §4.1 |
| **7** | **Minification filter** | 0 = point, 1 = linear | see §4.1 |
| **8** | **Diagonal LOD** | 0 = edge LOD, 1 = diagonal LOD | `_SHIFTL(!(edgelod&0xff),8,1)` — note the logical NOT |
| **16:9** | **LOD bias** | **signed 8-bit, 1/32 of a LOD per step** | `_SHIFTL(((u32)(32.0f*lodbias)),9,8)` |
| 18:17 | — | **Unverified:** never written by libogc. | |
| **20:19** | **Max anisotropy** | 0 = 1×, 1 = 2×, 2 = 4× | `_SHIFTL(maxaniso,19,2)` |
| **21** | **LOD / bias clamp** | 1 = enabled | `_SHIFTL(biasclamp,21,1)` |
| 23:22 | — | **Unverified:** never written. | |

Cross-checked: Dolphin's `BPMemory.h` splits `TexMode0` the same way — a one-bit magnification
filter at 4, a two-bit mipmap filter at 5, a one-bit minification filter at 7, a one-bit LOD type
at 8, a **signed** eight-bit LOD bias at 9, a two-bit max-anisotropy at 19 and a one-bit clamp at
21 — and its debug formatter prints the bias as `lod_bias / 32.f`, i.e. it independently agrees
on the 1/32 step.

**On the LOD bias being 1/32 and the LOD itself being 1/16.** This is the one place where the two
scales in this document meet, and it is easy to get wrong by a factor of two. libogc's
`GX_InitTexObjLODBias()` (`gx.c:4513-4521`) is the whole story on the register side:

```c
void GX_InitTexObjLODBias(GXTexObj *obj,f32 lodbias)
{
	struct __gx_texobj *ptr = (struct __gx_texobj*)obj;
	if(lodbias<-4.0f) lodbias = -4.0f;
	else if(lodbias==4.0f) lodbias = 3.99f;
	ptr->tex_filt = (ptr->tex_filt&~0x1fe00)|(_SHIFTL(((u32)(32.0f*lodbias)),9,8));
}
```

Eight signed bits at 1/32 per step is an **s2.5** fixed-point number with range
[−4.0, +3.96875]; the clamp to [−4.0, 3.99] at the top of the function is exactly the range that
fits. The LOD the hardware computes, by contrast, carries **four** fractional bits (§5.1). Dolphin
reconciles them by halving the bias before adding it, in both of its implementations and by two
different routes — the software rasteriser arithmetic-shifts the signed bias right by one, and the
shader path pre-multiplies the register value by 8 into 1/256 units when building its sampler
state and then shifts right by four. Both land on the same place: **the bias contributes
`lod_bias / 2` in units of 1/16 of a LOD, with the halving rounding toward −∞.**

A signed right shift, not a division: a bias register value of −1 (−1/32 of a LOD) contributes −1
in 1/16 units, i.e. −1/16, not −1/32 rounded to zero. Small, but it is a systematic downward bias
on odd negative values and it is free to get right.

**Bit 8 and the direction of the NOT.** libogc's API parameter is called `edgelod` and the
register bit is its inverse. `gx.h:4401-4411` describes the API side: with `GX_ENABLE` the LOD is
computed from adjacent texels, with `GX_DISABLE` from diagonal ones, and edge LOD should be
enabled whenever bias clamping or anisotropic filtering is in use. So:

| API `edgelod` | Register bit 8 | Name | What §3.3 does with it |
|---|---|---|---|
| `GX_ENABLE` (1) | **0** | edge LOD | reduce each axis with `max` |
| `GX_DISABLE` (0) | **1** | diagonal LOD | reduce each axis with `+` |

Dolphin's enumeration for this bit names 0 "Edge LOD" and 1 "Diagonal LOD", which agrees.

### 1.2 setmode1 (`0x84`–`0x87`, `0xA4`–`0xA7`) — the LOD range

`ptr->tex_lod`, built in `GX_InitTexObjLOD()` and in `GX_InitTexObjMinLOD()` / `GX_InitTexObjMaxLOD()`
(`gx.c:4493-4511`):

```c
	if(minlod<0.0f) minlod = 0.0f;
	else if(minlod>10.0f) minlod = 10.0f;
	ptr->tex_lod = (ptr->tex_lod&~0xff)|(((u32)(16.0f*minlod))&0xff);
	ptr->tex_lod = (ptr->tex_lod&~0xff00)|(_SHIFTL(((u32)(16.0f*maxlod)),8,8));
```

| Bits | Field | Units |
|---|---|---|
| 7:0 | Minimum LOD | **unsigned 8-bit, 1/16 of a LOD per step** |
| 15:8 | Maximum LOD | same |
| 23:16 | — | **Unverified:** never written. |

Both fields are **unsigned**, so neither end of the range can be negative: the hardware cannot be
told "never magnify less than −1". The representable range is 0 … 15.9375; libogc clamps its API
to 0 … 10.0 (register 0 … 160), and `gx.h:4107-4108` states the 0.0–10.0 range in the doxygen
too. Dolphin's formatter prints both as `value / 16.f`, agreeing on the step.

The units here are the same 1/16 the computed LOD carries, so min and max LOD are compared against
the LOD directly with no rescaling — unlike the bias.

**A libogc default worth knowing about.** `GX_InitTexObj()` opens with
`memset(obj,0,sizeof(GXTexObj))` and then never assigns `ptr->tex_lod`. A texture object that has
not had `GX_InitTexObjLOD()` or `GX_InitTexObjMaxLOD()` called on it therefore ships **min LOD 0
and max LOD 0** — level 0 only — even when the `mipmap` flag was true and the filter field was set
to trilinear. An emulator does not care; a reader trying to reason about what a libogc-built title
puts in the register does.

**Unverified:** whether Nintendo's own SDK `GXInitTexObj` also leaves the LOD range at zero, or
derives a default maximum from the texture dimensions. If it derives one, libogc's behaviour is a
divergence and commercial titles will have non-zero max LOD in the register even where the ported
homebrew equivalent would not. Nothing available here settles it; only a capture of a retail
title's BP stream would.

### 1.3 setimage0 and setimage3 — and the absence of per-level address registers

This is the single most important structural fact in the document, and it is easiest to state as a
correction to the obvious guess.

**There is exactly one texture address register per unit, and it points at level 0.**
setimage3 (`0x94`–`0x97`, `0xB4`–`0xB7`) holds the physical address of the texture data shifted
right by five, and that is all the addressing the hardware is given:

```c
	ptr->tex_maddr = (ptr->tex_maddr&~0x00ffffff)|(_SHIFTR(MEM_VIRTUAL_TO_PHYSICAL(img_ptr),5,24));
```

setimage0 (`0x88`–`0x8B`, `0xA8`–`0xAB`) holds width−1 in bits 9:0, height−1 in bits 19:10 and the
format in bits 23:20 — **of level 0 only**. There is no register anywhere in the BP file that
holds level 1's address, or level 1's width. Every other level's address and size is *derived*,
by the arithmetic in §2, from the level-0 address, the level-0 size and the format. That is why §2
matters more than anything else here: get the derivation wrong and the sampler reads the middle of
some other level, or the middle of a texel, and produces coloured noise rather than a blurry
picture.

libogc's `GX_GetTexObjAll()` doxygen says the same thing from the API side (`gx.h:4135-4136`): the
width and height it returns are "the width of the texture or LOD 0 for mipmaps".

### 1.4 setimage1 and setimage2 — even and odd levels live in different TMEM banks

The two TMEM configuration registers, whose layout the sibling document §8.4 gives, are
**setimage1 (`0x8C`–`0x8F`) for even LODs and setimage2 (`0x90`–`0x93`) for odd LODs**. libogc's
own parameter names make the split explicit: `GX_InitTexCacheRegion()` takes `tmem_even` and
`tmem_odd` and `gx.h:4214-4216` calls them "base ptr in TMEM for even LODs" and "for odd LODs".
Dolphin's header labels the corresponding bit fields the same way.

The reason is mechanical rather than architectural: a trilinear fetch needs two levels at once, so
the two levels a blend touches must be in banks that can be read in the same cycle. Consecutive
levels are always one even and one odd, so a two-bank split with the parity as the selector always
gives a conflict-free pair. libogc's own performance-counter documentation puts a number on the
worst case (`gx.h:1094-1095`): for a mipmap, as many as **eight cache lines** may be touched to
produce one textured pixel — four texels from each of two levels.

**A reimplementation that reads main memory directly can ignore all of this.** TMEM is a cache;
the texels in it came from the addresses §2 computes. The even/odd split only becomes load-bearing
if the implementation models TMEM residency, preloaded (non-auto-fetched) regions, or the
`GX_PreloadEntireTexture()` path, which walks the chain writing alternate levels to the two banks.

### 1.5 The SU scale registers (`0x30`–`0x3F`) — where the texture size enters

The LOD formula in §3 contains no reference to the texture's dimensions. That is not an omission:
**the scale by texture size happens upstream, at the texture coordinate scale registers, before
rasterisation.** By the time the derivatives are taken, the coordinate is already in texels.

The sibling document §8.5 gives the registers: texture coordinate *n* (not texture map *n*) uses
`0x30 + 2n` for S and `0x31 + 2n` for T, bits 15:0 of each holding the **scale minus one**, which
`__SetSURegs()` fills from the bound map's width−1 and height−1.

Dolphin behaves as if the transform unit, immediately after texgen and before anything else,
multiplies each generated coordinate's S by `(s_scale_minus_1 + 1)` and its T by
`(t_scale_minus_1 + 1)` — that is, by the level-0 width and height in texels. Everything
downstream — the interpolated per-pixel coordinate, the quad derivatives, the LOD, and the sampler
input — is in units of level-0 texels.

Two consequences worth spelling out:

* A coordinate of 1.0 on a 64-texel axis arrives at the rasteriser as 64.0, and a screen-space
  derivative of 1.0 means "one level-0 texel per pixel", which is LOD 0. The formula in §3.4 needs
  no `+ log2(width)` term because the term is already folded in.
* `GX_SetTexCoordScaleManually()` overrides that scale. When a title uses it — the documented
  reason is indirect tiled textures, see [`gc-fog-indirect.md`](gc-fog-indirect.md) §B.4 — **the
  LOD is computed against the manual scale, not against the texture's real size.** An
  implementation that recovers the texel-space coordinate by multiplying normalised coordinates by
  the bound texture's width will silently disagree with hardware exactly in those cases.

### 1.6 Two LOD inputs that are not texture registers

**Field mode (`0x68`).** libogc's `GX_SetFieldMode()` doxygen (`gx.h:3559-3571`) states the
problem plainly: in field-mode rendering a pixel covers only half the vertical space between
rendered scan lines, because the other half belongs to the other field, and "one must adjust the
vertical part of the texture LOD computation" to compensate. Dolphin's header carries a one-bit
`texLOD` field at bit 0 of register `0x68` with a comment to the same effect.

**Unverified:** the adjustment itself. Halving the vertical derivative (equivalently subtracting
½ a LOD when the vertical axis is the one that wins the reduction) is the only adjustment that
makes geometric sense, but neither Dolphin implementation applies the field-mode bit to the LOD at
all, so no source read for this document performs any adjustment and the factor is a deduction
from the prose, not a description of an implementation.

**Indirect `utclod` (`0x10`–`0x1F` bit 19).** An indirect stage can choose whether the LOD is
computed from the coordinate as the texgen produced it or as the indirect matrix modified it.
`gx.h:3212` calls it "whether to [use] the unmodified (`GX_TRUE`) or modified (`GX_FALSE`) tex
coords for mipmap LOD computation". This is covered in
[`gc-fog-indirect.md`](gc-fog-indirect.md) §B.3 and §B.4 and is not restated here; the point for
this document is only that **the coordinate whose derivative §3 takes is not always the coordinate
the sampler is finally given**, and an implementation that computes both from one variable will
get tiled and warped textures wrong.

---

## 2. The mip chain in memory

### 2.1 Level dimensions

Level *n*'s dimensions are level 0's shifted right by *n*, floored at one texel:

```
W(n) = max(1, W >> n)
H(n) = max(1, H >> n)
```

This is the shape of libogc's two mip loops (the sibling document quotes them at `gx.c:3582-3583`
and `gx.c:3128-3131` in its own numbering) and Dolphin derives its per-level raw dimensions the
same way, shifting the parent's dimension by the level index with a floor of one.

Note that the two axes floor **independently**. A 64 × 8 texture's level 4 is 4 × 1 and its level
6 is 1 × 1; the chain does not stop when the shorter axis reaches one, and levels past that point
are not square.

### 2.2 A level's size in bytes, and the rounding that decides it

From the formats document §1 and §2, a format has a tile width `TW`, a tile height `TH` and a
bytes-per-tile `TS` — 32 for every format but `RGBA8`, which is 64. A level occupies

```
level_size(w, h) = ceil(w / TW) * ceil(h / TH) * TS
```

**The ceiling is not optional and it is where garbage comes from.** Two separate roundings hide in
it and both bite at the tail of a chain:

* A level narrower than one tile still costs a whole tile. A 1 × 1 level of a 4 × 4-tile format
  occupies 32 bytes, not 2; a 1 × 1 `CMPR` level occupies 32 bytes, not 4; a 1 × 1 `RGBA8` level
  occupies 64 bytes.
* A level whose width is not a multiple of the tile width is padded to a whole tile, and the
  padding occupies real bytes that the next level's offset must step over.

Dolphin computes the same quantity by rounding each raw dimension **up** to the format's block
width and height and sizing the result, which is the same expression. Its software rasteriser
takes a shortcut — it clamps each dimension up to the block size with a maximum rather than
aligning up to a multiple of it — which coincides with the correct answer for every power-of-two
level and differs only for dimensions that are neither powers of two nor multiples of the tile.
Since mipmapped textures must be power-of-two anyway (§2.5), the two agree in every legal case.
Implement the ceiling, not the clamp: it is no harder and it degrades gracefully.

### 2.3 Offsets: levels are contiguous, and that is the whole addressing scheme

```
offset(0) = 0
offset(n) = offset(n-1) + level_size(W(n-1), H(n-1))
address(n) = base + offset(n)
```

with `base` the setimage3 payload shifted left by five. No alignment padding is inserted between
levels beyond each level's own rounding up to whole tiles.

The evidence is libogc's mip-chain walker in `GX_PreloadEntireTexture()`, which the sibling
document quotes (`gx.c:3542`, `gx.c:3594` in its numbering): the main-memory address advances by
exactly the previous level's tile count in 32-byte units, doubled for `RGBA8`, and the tile count
is then recomputed for the next level from its own dimensions. Dolphin arrives at the same layout
independently, walking a span forward by each level's byte size as it constructs the chain.

Two worked examples, because an off-by-one-tile here is invisible on level 0 and catastrophic on
level 3.

**64 × 32 `RGB5A3`** (4 × 4 tile, 32 bytes per tile):

| Level | W × H | Tiles | Bytes | Offset |
|---|---|---|---|---|
| 0 | 64 × 32 | 16 × 8 = 128 | 4096 | 0 |
| 1 | 32 × 16 | 8 × 4 = 32 | 1024 | 4096 |
| 2 | 16 × 8 | 4 × 2 = 8 | 256 | 5120 |
| 3 | 8 × 4 | 2 × 1 = 2 | 64 | 5376 |
| 4 | 4 × 2 | 1 × 1 = 1 | 32 | 5440 |
| 5 | 2 × 1 | 1 × 1 = 1 | 32 | 5472 |
| 6 | 1 × 1 | 1 × 1 = 1 | 32 | 5504 |

Total 5536 bytes. The last three levels cost the same 32 bytes each — the chain does not shrink
geometrically at the tail, and a naïve `size / 3` estimate of the mip overhead is wrong in the
direction that truncates.

**64 × 64 `CMPR`** (8 × 8 tile, 32 bytes per tile):

| Level | W × H | Tiles | Bytes | Offset |
|---|---|---|---|---|
| 0 | 64 × 64 | 8 × 8 = 64 | 2048 | 0 |
| 1 | 32 × 32 | 4 × 4 = 16 | 512 | 2048 |
| 2 | 16 × 16 | 2 × 2 = 4 | 128 | 2560 |
| 3 | 8 × 8 | 1 × 1 = 1 | 32 | 2688 |
| 4 | 4 × 4 | 1 × 1 = 1 | 32 | 2720 |
| 5 | 2 × 2 | 1 × 1 = 1 | 32 | 2752 |
| 6 | 1 × 1 | 1 × 1 = 1 | 32 | 2784 |

Total 2816 bytes, of which the four tail levels are 128 — more than level 2 costs.

**Do not use `GX_GetTexBufferSize()` to compute offsets.** The formats document §7.2 records that
it sizes the three paletted formats with the wrong tile geometry, over-estimating by 4× for `C4`
and 2× for `C8`. It is safe as an allocator and wrong as an address generator.

### 2.4 Within a level, nothing changes

Once the level's base address and its own `W(n)`, `H(n)` are in hand, the texel address is
computed exactly as the formats document §5.1 describes, with the *level's* width in the
`tiles_per_row` term — not level 0's. The decode of the texel is unchanged. A mipmapped texture is
a sequence of ordinary textures sharing a format and an allocation.

For the paletted formats the palette is shared across the whole chain: the TLUT binding register
is per texture unit, not per level.

### 2.5 How many levels exist, and the power-of-two requirement

Nothing in memory marks the end of a chain. Two bounds exist and they are independent:

* **The geometric bound.** `floor(log2(max(W, H)))` levels follow the base, so the chain is at
  most `floor(log2(max(W, H))) + 1` levels long. libogc computes exactly this as
  `31 - cntlzw(max(W, H))` (sibling document, `gx.c:3517-3520`).
* **The register bound.** The max LOD field. Dolphin derives its level count as
  `ceil(max_lod / 16)` additional levels — the max LOD in whole levels, rounded up, because a
  fractional max LOD still permits a blend that touches the level above — and then clamps that to
  the geometric bound.

An implementation should carry both and take the smaller. The consequence of taking only the
register bound is reading past the end of the allocation for the deepest levels of a small
texture, which is exactly the "garbage rather than a picture" failure this document exists to
prevent.

**Mipmapped textures must be power-of-two in both axes.** `gx.h:4263-4264` and `gx.h:4295-4296`
state it as an API precondition — "max value is 1024; mipmaps must be a power of two" — and
`gx.h:4149` repeats it for `GX_GetTexBufferSize()`. They need not be square. The formats document
§6.2 records the related, separate requirement that `GX_REPEAT` and `GX_MIRROR` need power-of-two
dimensions even without mipmaps.

**Unverified:** what the hardware does when a chain is shorter than max LOD claims. The
plausible behaviour is that it simply reads whatever follows the allocation and decodes it as
texels, since nothing in the pipeline knows where the chain ends — but nothing read for this
document either confirms that or describes a clamp, and the two Dolphin paths avoid the question
by clamping the level count in software before sampling.

---

## 3. Computing the LOD

Everything from here to §5 is Dolphin behaviour rather than quoted libogc. Where its two
implementations agree the claim is firm enough to build on; where they differ it is marked.

### 3.1 The 2 × 2 quad, and yes, that is where the derivatives come from

The Flipper rasterises in **2 × 2 pixel quads**, and libogc's own documentation says so
repeatedly — the quad performance metrics exist to count them, and `gx.h:1045-1047` defines the
term: "the quad metrics allow you to count the number of quads (2x2 pixels) the GP processes",
with *coverage* being how many of the four pixels actually lie inside the triangle, counted
separately for coverage 0, 1, 2, 3 and 4. Two other parts of the pipeline are documented as
operating per quad rather than per pixel: fog range adjustment is "computed per quad (2x2 pixels),
not per-pixel" (`gx.h:3296`), and the Z of a quad is normally a reference Z plus two slopes
(`gx.h:2766`).

**Dolphin behaves as if the LOD derivatives are finite differences across that quad.** Its
software rasteriser builds the block of interpolated coordinates first and then takes the LOD from
three of its corners — the quad's origin pixel, its horizontal neighbour and its vertical
neighbour — giving one LOD for the whole quad rather than one per pixel. Its shader path uses the
hardware's own screen-space derivative instructions, which are themselves quad differences, so the
two agree structurally.

With `(u, v)` the per-pixel texture coordinate **already in level-0 texels** (§1.5), the four
quantities are the absolute finite differences

```
dudx = |u(1,0) - u(0,0)|      dvdx = |v(1,0) - v(0,0)|
dudy = |u(0,1) - u(0,0)|      dvdy = |v(0,1) - v(0,0)|
```

taken *after* the perspective divide, not before — the coordinate differenced is the projected
one, so the derivative correctly grows towards the horizon.

**Unverified:** whether the hardware really differences the quad's corner pixels, and which
corner it uses as the origin. The quad structure is documented by libogc; the use of quad
differences *for LOD specifically* is Dolphin's behaviour, matching what every quad-rasterising
GPU does, but it is an inference from architecture rather than a measurement. A pixel-by-pixel
difference in LOD across a quad boundary would settle it (§8, test 1).

**Unverified:** whether the absolute values are taken per-term as above, or whether the hardware
keeps signs until later. Dolphin takes them immediately. For the `max` reduction it makes no
difference; for the `+` reduction of diagonal LOD it does — a coordinate that increases in x and
decreases in y would give a larger sum with per-term absolute values than with signed addition.

### 3.2 The two reductions: edge LOD and diagonal LOD

Setmode0 bit 8 (§1.1) selects between them. Dolphin behaves as if:

| Bit 8 | Name | Reduction |
|---|---|---|
| 0 | **edge LOD** | `sDelta = max(dudx, dudy)`, `tDelta = max(dvdx, dvdy)` |
| 1 | **diagonal LOD** | `sDelta = dudx + dudy`, `tDelta = dvdx + dvdy` |

Both are cheap approximations to the length of the pixel's footprint in texture space. Edge LOD
takes the longer of the two axis-aligned edges of the footprint parallelogram; diagonal LOD takes
the L1 length of its diagonal, which is never smaller and is usually larger, so **diagonal LOD
biases towards blurrier levels** and edge LOD towards sharper ones. Neither computes a true
Euclidean length; there is no square root anywhere in this pipeline.

That relationship explains libogc's advice at `gx.h:4404-4406`: edge LOD should be enabled
whenever bias clamping or anisotropic filtering is in use. Both of those features exist to
*recover* sharpness in the minor axis of a stretched footprint, and diagonal LOD's systematic
over-estimate would work against them.

### 3.3 The reduction to one number, and the log2

The two per-axis deltas collapse to one:

```
delta = max(sDelta, tDelta)
LOD   = log2(delta)
```

`delta` is in texels per pixel, so `delta == 1` gives LOD 0 and each doubling adds one — which is
the whole reason the texture size had to be folded in upstream (§1.5).

The log2 is not exact. Dolphin behaves as if the hardware takes the classic bit-twiddling
approximation from the IEEE-754 representation of `delta`: the biased exponent supplies the
integer part and the **top four bits of the mantissa are used directly as the fraction**, i.e.
for `delta = 2^e * (1 + m)` with `m` in [0, 1) it computes `e + m` rather than
`e + log2(1 + m)`. Its implementation extracts both fields with a single shift of the float's bit
pattern and subtracts the exponent bias pre-scaled by sixteen, producing a result in **s28.4**
directly.

The approximation is exact at `m = 0` and `m → 1` and sags in between, under-estimating the LOD by
at most about 0.086 of a level near `m ≈ 0.44`. In 1/16 units that is a maximum error of about 1.4
steps — visible as a slightly-too-sharp band in the middle of each octave of a trilinear gradient,
and precisely the kind of thing a hardware test can see (§8, test 3).

**Unverified: whether the hardware's log2 is this approximation or an accurate log2 truncated to
1/16.** This is the single most consequential open item in the document. Dolphin's software
rasteriser uses the approximation; its shader path calls the GPU's own `log2` and scales by 16,
which is accurate to well within a step. So Dolphin's two implementations *disagree*, and Dolphin
itself evidently does not regard the distinction as settled. Either is plausible for 2001 silicon;
the approximation is nearly free in hardware and the exact function is not.

A degenerate input needs handling: `delta == 0` — a pixel whose texture coordinate does not change
at all, which happens on a coordinate held constant across a primitive. Dolphin's shader path
substitutes the minimum LOD; its software path lets the bit trick run on a zero float, which
yields an enormous negative number that the subsequent clamp lifts to the minimum LOD anyway. The
two therefore agree on the outcome, which is: **`delta == 0` reads level `min_lod` with the
magnification filter.**

### 3.4 Bias, then clamp

In order:

```
lod = FixedLog2(delta)                    /* s28.4, four fractional bits */
lod = lod + (lod_bias >> 1)               /* arithmetic shift; §1.1 */
lod = min(lod, max_lod)                   /* max_lod and min_lod are already in 1/16 */
lod = max(lod, min_lod)
```

Both Dolphin implementations add the bias before clamping, which is also what libogc's
documentation describes from the API side (`gx.h:4378-4380`): the bias "is added to the computed
lod and the result is clamped between the values given to `GX_InitTexObjMinLOD()` and
`GX_InitTexObjMaxLOD()`".

The clamp order is `max` first then `min` in Dolphin's software path, which matters only when
`min_lod > max_lod` — a configuration libogc's API does not prevent. In that case testing against
the maximum first and the minimum second leaves the minimum winning. Dolphin's sampler-state
builder for the shader path instead pre-clamps `min_lod` to `max_lod`, giving the opposite
resolution. **Unverified:** which the hardware does. It is a pathological configuration and
probably appears in no shipped title.

Note that because both LOD-range fields are unsigned (§1.2), **a positive `min_lod` makes
magnification impossible**: the clamp lifts every LOD to at least `min_lod`, which is greater than
zero, and §4.2's rule then selects the minification filter for every pixel including ones that are
magnifying badly.

### 3.5 Bias clamp (bit 21)

Neither Dolphin path implements this bit. Its software rasteriser does not read it; its shader
generator extracts it into a variable and never uses it. So this section rests on libogc's prose
alone.

`gx.h:4391-4393` describes it: with bias clamping enabled, the sum of the LOD and the bias "is
clamped so that it is never less than the minimum extent of the pixel projected in texture space",
which "prevents over-biasing the LOD when the polygon is perpendicular to the view direction".
`gx.h:4380` adds the observable consequence: the effect of the bias "will diminish as the polygon
becomes more perpendicular to the view direction".

The reading that fits both sentences: the ordinary LOD is `log2` of the **major** extent of the
projected pixel footprint (§3.3 takes a `max`), and bias clamp installs a floor at `log2` of the
**minor** extent — the other one of the two deltas. On a polygon seen face-on the two extents are
equal, the floor equals the unbiased LOD, and a negative bias is suppressed entirely; on a polygon
seen edge-on the extents diverge, the floor falls away below the LOD, and the bias applies in
full. That is exactly libogc's second sentence, given that a polygon "perpendicular to the view
direction" is one whose normal points at the viewer — a face-on polygon, the case where the two
extents coincide. The two statements agree, and the formula they agree on is:

```
if (bias_clamp) lod = max(lod, log2(min(sDelta, tDelta)))
```

**Unverified: the bias clamp's exact rule.** The formula above is this document's own inference
from two sentences of libogc doxygen plus the shape of §3.3, and nothing read for this document
implements the bit. The direction of the effect — that it can only raise the LOD, never lower it,
and that it only binds when the bias is negative — follows from "never less than" and is the part
worth trusting. Note that libogc advises enabling edge LOD alongside it (`gx.h:4405-4406`), which
is consistent with a rule that needs both per-axis deltas to be meaningful rather than summed
together.

### 3.6 Anisotropy (bits 20:19)

`gx.h:4417-4424` describes the mechanism: anisotropic filtering iterates the square filter along
the direction of anisotropy to approximate the quadrilateral footprint, the hardware uses multiple
cycles "only when necessary", the maximum number of cycles is clamped by the setting so that
`GX_ANISO_2` costs at most two filter cycles per texture, and the extra cycles are internal to the
texture filter and do not consume TEV stages. It adds two configuration requirements: the
minification filter should be `GX_LIN_MIP_LIN` when anisotropy is above 1×, and edge LOD should be
enabled.

**Unverified: everything about how the samples are placed and weighted.** Neither Dolphin path
implements anisotropy — the software renderer ignores the field and the shader generator carries
an explicit note that it is unimplemented — and libogc's prose gives a mechanism without a
formula. What a from-scratch implementation can safely take from it: the number of filter cycles
is at most 1, 2 or 4 by the field value; the iteration is along the major axis of the footprint;
the result is an average of square (bilinear or trilinear) probes. Where the probes sit, how many
are actually used for a given anisotropy ratio, and whether the LOD used for the probes is the
minor-axis LOD rather than the major-axis one, are all open.

The pragmatic choice for a first implementation is to treat all three values as 1× and accept
slightly blurrier ground planes. That is a visible difference from hardware and should be recorded
as one, but it is a *smooth* difference, unlike getting §2 wrong.

---

## 4. From LOD to a filter

### 4.1 Decoding the three filter bits

Bits 7:5 are written as one three-bit value by libogc, mapped from the API's six filter modes
through a table (`gx.c:104`, `gx.c:4486-4491`):

```c
static const u8 _gx2HWFiltConv[] = {0x00,0x04,0x01,0x05,0x02,0x06,0x00,0x00};
...
	ptr->tex_filt = (ptr->tex_filt&~0xe0)|(_SHIFTL(_gx2HWFiltConv[minfilt],5,3));
```

Reading that table against the enumeration at `gx.h:1000-1005`:

| API mode | Value | Table | Bit 7 (min filter) | Bits 6:5 (mip mode) |
|---|---|---|---|---|
| `GX_NEAR` | 0 | 0x00 | point | none (0) |
| `GX_LINEAR` | 1 | 0x04 | linear | none (0) |
| `GX_NEAR_MIP_NEAR` | 2 | 0x01 | point | point (1) |
| `GX_LIN_MIP_NEAR` | 3 | 0x05 | linear | point (1) |
| `GX_NEAR_MIP_LIN` | 4 | 0x02 | point | linear (2) |
| `GX_LIN_MIP_LIN` | 5 | 0x06 | linear | linear (2) |

So the two axes are orthogonal in the register even though the API names them as six modes: **bit
7 says how the four texels *within* a level are combined, bits 6:5 say how many levels are read
and how they are combined.** Entries 6 and 7 of the table are zero, i.e. libogc silently maps
out-of-range filter values to `GX_NEAR`.

The magnification filter is a separate single bit at 4, always point or linear, never a mip mode.
The API enforces that too: `gx.h:4573` says the magnification parameter must be `GX_NEAR` or
`GX_LINEAR`. This is structural, not a convenience — see §4.3.

Corroboration from libogc's direct-construction paths, quoted in the sibling document §8.1: a
non-mipmapped texture object gets `0x0080` (bit 7 set, mip mode 0), a mipmapped paletted texture
gets `0x00a0` (mip mode 1) and any other mipmapped texture gets `0x00c0` (mip mode 2). The
paletted case is not an accident: `gx.h:4282-4284` states that colour-index mipmaps cannot use
`GX_LIN_MIP_LIN` or `GX_NEAR_MIP_LIN`, i.e. **a paletted texture cannot be trilinearly filtered.**
Blending two levels of indices before the palette lookup would be meaningless, and blending after
would need two lookups per level.

### 4.2 Magnification versus minification

The rule is a single comparison, and it is on the **final LOD, not on the raw ratio**:

> **`lod > 0` selects the minification filter (bit 7). `lod <= 0` selects the magnification filter
> (bit 4).**

Both Dolphin paths use exactly this test, including the tie: **LOD exactly 0 magnifies**. With the
LOD in 1/16 units the comparison is against the integer 0, so a LOD of 1/16 already minifies.

Cross-checking against libogc's API documentation, which describes the boundary from the other
direction: the minification filter applies "when the texel/pixel ratio is >= 1.0" and the
magnification filter "when the texel/pixel ratio is < 1.0" (`gx.h:4572-4573`, and the same wording
at `gx.h:4351-4352`). A ratio of exactly 1.0 is LOD exactly 0, and libogc puts that case in the
*minification* bucket while both Dolphin paths put it in the magnification bucket. The two are
describing the same boundary and disagreeing about which side the boundary itself falls on.

**Unverified: the tie at LOD 0.** It is one texel wide in the worst case and invisible in
practice, but it is cheap to note and cheap to test (§8, test 4).

**Unverified: whether the test runs before or after the min/max clamp.** Dolphin's two
implementations differ here and it is not a subtle difference. The software rasteriser decides
magnification against the **biased but unclamped** LOD and then clamps; the generated shader
clamps first and decides against the **clamped** value. They diverge whenever `min_lod > 0`: on a
magnifying pixel with `min_lod = 1.0`, the software path uses the magnification filter and the
shader path uses the minification filter. Since `min_lod` defaults to zero and few titles raise
it, this may never have been noticed. It is directly testable (§8, test 5).

Note what the rule does *not* say. The magnification filter has no mip mode, so **a magnifying
pixel always reads level 0** — or, strictly, level `min_lod`, since the clamp has already been
applied to the LOD by the time the level index is taken. There is no such thing as magnifying
level 3.

### 4.3 The mip mode

With the final LOD in hand, split it:

```
base_level = lod >> 4        /* arithmetic shift: negative LOD gives level 0 after §4.2 */
frac       = lod & 15
```

and then, by mip mode:

| Mip mode | Behaviour |
|---|---|
| 0 — none | Read `base_level` only. One bilinear or point fetch. |
| 1 — point | Read one level, rounded to nearest: `base_level + (frac >= 8 ? 1 : 0)`. |
| 2 — linear | Read `base_level` and `base_level + 1` and blend by `frac` (§5.3). |

Dolphin behaves as if all of this is gated on `lod > 0` as well — mip modes 1 and 2 do nothing
when the pixel is magnifying, which follows from §4.2 but is worth stating separately because it
means a trilinear texture on a magnifying surface performs exactly one bilinear fetch of level 0,
not a blend of level 0 with itself.

Round-to-nearest for mip mode 1, not round-down: `frac >= 8` is a half-LOD threshold, so a LOD of
2.5 reads level 3. That is the conventional choice but not the only possible one; it comes from
Dolphin's software path, and its shader path applies the same `frac >= 8` rounding, so both agree.

---

## 5. Precision and rounding

### 5.1 The LOD: s28.4

The computed LOD carries **four fractional bits** — steps of 1/16 — and this is the number the min
LOD and max LOD registers are expressed in (§1.2), so that clamp is exact and needs no rescaling.
The LOD bias register carries **five** fractional bits (§1.1) and is halved into the LOD's scale
before use, which is the only rescaling anywhere in the chain.

The integer part has far more range than it can use: a 1024-texel texture has at most 11 levels,
and the max LOD register saturates at 15.9375 anyway.

### 5.2 The texture coordinate: s17.7

Dolphin behaves as if the coordinate handed to the sampler is a **fixed-point number with seven
fractional bits** — 1/128 of a texel — obtained by scaling the interpolated floating-point
coordinate by 128. Seven bits of sub-texel precision is what bounds the quality of the bilinear
blend within a level.

The LOD itself, though, is computed from the *floating-point* coordinate in Dolphin's software
path, before that quantisation. So the derivative retains full precision even where the sample
position does not.

**Unverified:** whether the hardware computes the derivative from the full-precision coordinate or
from the quantised one. It matters at very small derivatives — below 1/128 of a texel per pixel,
a quantised difference is either zero or one step, which would make the LOD jump in coarse
increments for heavily magnified surfaces rather than sliding smoothly. Since those are all
magnifying pixels that read level 0 regardless, the visible consequence is nil, and the question
is academic unless `min_lod` is raised.

### 5.3 The trilinear blend: four bits of weight

With `frac` the LOD's four fractional bits (§4.3), Dolphin behaves as if each channel is combined
as

```
result = (lower_level * (16 - frac) + upper_level * frac) >> 4
```

with the two operands being the already-filtered 8-bit results from each level and the shift being
a truncation, not a round. So the weight has sixteen positions, `frac == 0` returns the lower
level exactly, and no value of `frac` returns the upper level exactly — the blend is asymmetric at
the top, which is the usual consequence of using `n` and `16 - n` over sixteen steps rather than
fifteen. An implementation that interpolates in floating point and rounds will differ from this by
up to one unit of the result on most pixels.

### 5.4 The bilinear blend within a level: seven bits of weight

For completeness, because the runtime's existing magnifying filter marks this precision as
unverified in a comment and this is the source that settles it. Dolphin behaves as if the four
texels of a bilinear fetch are combined in one step with the product weights

```
(128 - fs) * (128 - ft),  fs * (128 - ft),  (128 - fs) * ft,  fs * ft
```

where `fs` and `ft` are the low seven bits of the fixed-point S and T coordinates, and the sum of
the four weighted channels is shifted right by **fourteen** — the two seven-bit weights multiplied
together. Again a truncation. The existing float implementation in
`runtimes/runtime-gc/src/gc_gx_texture.c` adds 0.5 before truncating, i.e. rounds, and will therefore
differ by one unit from this on roughly half of all filtered texels.

Both of these are Dolphin's arithmetic, not a hardware measurement, so the note in that file
should be softened rather than deleted: the *format* (7-bit coordinate fraction, 14-bit
denominator, truncation) now has a source; the claim that the hardware truncates rather than
rounds does not.

### 5.5 Getting the coordinate into the level's own space

The sampler's coordinate is in level-0 texels (§1.5), and each level is half the size of the one
before, so sampling level *n* needs the coordinate divided by `2^n`. Dolphin behaves as if it does
this by arithmetic-shifting the whole fixed-point coordinate right by the level index:

```
s_level = s >> n
t_level = t >> n
```

and then wrapping against **the level's own dimensions**, `W(n)` and `H(n)`, not against level
0's — which is the only sensible choice given that the register only holds level 0's size, and
which is what makes a repeated texture tile identically at every level.

Note the precision consequence of shifting the fixed-point value rather than rescaling it: level
*n* keeps only `7 - n` fractional bits, so by level 7 the bilinear fraction is gone entirely and
the filter degenerates to point sampling. Deep levels are tiny and blurry, so this is invisible;
it is recorded because a reimplementation that keeps full precision at every level will produce
very slightly different results at the tail of the chain and should not be surprised by it.

---

## 6. The whole thing, in order

A sampling of one pixel, with every step's source marked — **(L)** for quoted libogc, **(D)** for
Dolphin behaviour, **(?)** for an item in §7.

1. Texgen produces `(s, t)` and the SU scale registers multiply them by level-0 width and height
   **(L for the registers, D for the placement of the multiply)**.
2. The rasteriser interpolates them across a 2 × 2 quad and divides by w **(D)**.
3. Four absolute finite differences across the quad give `dudx, dvdx, dudy, dvdy` **(D, ?)**.
4. Setmode0 bit 8 reduces each axis: `max` for edge LOD, `+` for diagonal LOD **(L for the bit, D
   for the reduction)**.
5. `delta = max(sDelta, tDelta)`; `lod = log2(delta)` in s28.4 **(D, ? on the log2's exactness)**.
6. `lod += lod_bias >> 1` **(L for the units, D for the halving)**.
7. If bit 21, floor the LOD at `log2(min(sDelta, tDelta))` **(?)**.
8. Clamp to `[min_lod, max_lod]` from setmode1 **(L for the fields, D for the order)**.
9. `lod > 0` picks the minification filter, else the magnification filter **(D, ? on the tie and
   on whether this precedes step 8)**.
10. `base = lod >> 4`, `frac = lod & 15`; mip mode picks one level, the nearer level, or two
    **(L for the mode encoding, D for the rounding)**.
11. Each level's address is `base_address + offset(level)` from §2.3 **(L)**.
12. The coordinate is shifted right by the level index and wrapped against the level's own size
    **(D)**.
13. Point or bilinear within each level, per bit 7 or bit 4 **(L)**.
14. If two levels, blend by `frac` over sixteen **(D)**.

Steps 1, 10 (partly), 11 and 13 are the ones that decide whether there is a picture at all. Steps
3–9 decide whether it shimmers.

---

## 7. What could not be determined

| # | Item | § | Rests on | What is missing |
|---|---|---|---|---|
| 1 | Whether the LOD log2 is the mantissa-linear approximation or an accurate log2 | 3.3 | Dolphin's two paths **disagree** | A measurement of the level boundary position within an octave |
| 2 | Whether the mag/min test runs before or after the min/max clamp | 4.2 | Dolphin's two paths **disagree** | A draw with `min_lod > 0` on a magnifying surface |
| 3 | Whether LOD exactly 0 magnifies or minifies | 4.2 | Dolphin says magnify, libogc's wording implies minify | A 1:1 textured quad with different point/linear settings in the two filter fields |
| 4 | The bias clamp rule (bit 21) | 3.5 | libogc doxygen prose only; **nothing implements it** | Any measurement at all |
| 5 | Anisotropic filtering: sample placement, count and weights | 3.6 | libogc doxygen prose only; **nothing implements it** | Any measurement at all |
| 6 | The field-mode LOD adjustment (`0x68` bit 0) | 1.6 | libogc doxygen prose only; **nothing implements it** | A field-rendered draw with a known LOD |
| 7 | That the derivatives are quad finite differences, and from which corner | 3.1 | Quad rasterisation is documented (L); the LOD's use of it is D | A LOD discontinuity at a quad boundary |
| 8 | Whether per-term absolute values precede the diagonal-LOD sum | 3.1 | D | A surface whose u increases in x and decreases in y |
| 9 | The tie-break when `min_lod > max_lod` | 3.4 | Dolphin's two paths **disagree** | A deliberately inverted range |
| 10 | Whether the hardware truncates or rounds the trilinear and bilinear blends | 5.3, 5.4 | D | A single-texel-difference gradient read back from the EFB |
| 11 | Whether the derivative uses the full-precision or the 1/128-quantised coordinate | 5.2 | D | Only observable with `min_lod > 0` |
| 12 | What happens when max LOD names a level past the end of the allocation | 2.5 | Neither; both Dolphin paths clamp in software | A short chain with an over-long max LOD |
| 13 | setmode0 bits 18:17 and 23:22, setmode1 bits 23:16 | 1.1, 1.2 | Never written by libogc; absent from Dolphin's header | A write-then-read of the BP file, if one is possible |
| 14 | Whether Nintendo's SDK defaults the LOD range to zero as libogc does | 1.2 | libogc source only | A retail title's BP stream |

Items 1 and 2 are the ones to close first: both change output on ordinary content, and both are
places where the only available reference contradicts itself.

Items 4 and 5 are the ones to close *last*. Both features are rare in shipped content, both have
smooth failure modes — a texture slightly blurrier than hardware, never a wrong texture — and
neither has any implementation to compare against, so a test for them starts from nothing.

Everything in §1, §2 and §4.1 is derived from quoted libogc expressions and is safe to implement
as written.

---

## 8. Hardware tests that would settle these

Each of these is a small GX program that draws into the EFB, copies it out and hashes or dumps the
result, in the style the `gxtest` programs already in `roms/gc/hwtests/` use. None needs anything
the existing harness cannot do. They are ordered by value.

**1. The LOD ramp — settles items 1, 3, 7 and gives a reference curve for everything else.**
Build a 256 × 1 texture whose mip chain is flat-coloured per level: level 0 pure red, level 1 pure
green, level 2 pure blue, and so on through a palette with no two levels alike. Set mip mode to
*point* and both filters to point, min LOD 0, max LOD 10, bias 0. Draw a full-width quad whose
texture coordinate ramps such that the minification ratio sweeps continuously from 1:1 to 1:256
across the screen. Copy out one scanline.

The output is a staircase, and the position of each riser is the LOD at which `frac` crossed 8.
With an accurate log2 the risers sit at geometrically spaced x positions; with the mantissa-linear
approximation they sit up to 0.086 of a level early, which on a 256-pixel sweep is a shift of
several pixels — comfortably resolvable. Reading the risers also gives the exact screen position
at which LOD crosses 0, which is item 3, and running the same sweep at two sub-pixel vertical
offsets exposes whether the LOD changes per pixel or per 2 × 2 quad, which is item 7.

**2. The same ramp with mip mode linear — settles item 10 and confirms §5.3.** Make level 0 black
and level 1 white and nothing else, cap max LOD at 1.0, and sweep the ratio from 1:1 to 1:2. The
output is a 17-step grey ramp if the blend has four fractional bits, and the exact step values say
whether the combine truncates or rounds. Repeating with level 0 white and level 1 black checks the
asymmetry at `frac == 0` and `frac == 15` described in §5.3.

**3. Bias units and the halving — confirms §1.1, and is the cheapest sanity check here.** Run test
1 unchanged but with LOD bias set to each of +1/32, +2/32, +16/32 and −1/32. The staircase should
translate by exactly the bias in LOD, which on the ramp is a known number of pixels. A bias of
+1/32 that moves the staircase by the same amount as +2/32 confirms the arithmetic-shift halving
and its round-toward-−∞ behaviour on the odd value; a bias of +1/32 that moves it by twice as much
as expected means the LOD carries five fractional bits rather than four and §5.1 is wrong.

**4. min LOD versus the magnification test — settles item 2.** Draw a 1:1 quad (LOD 0, magnifying
by the tie rule) with `min_lod = 1.0`, the magnification filter set to *linear* and the
minification filter set to *point*, using a texture whose level 1 differs obviously from its level
0. Four distinguishable outcomes: level 0 filtered linearly (the test precedes the clamp and the
magnification filter also forces level 0), level 1 point-sampled (the test follows the clamp),
level 1 filtered linearly, or level 0 point-sampled. Each outcome names a different combination of
the clamp order and the level the magnification path reads, so the test resolves two questions at
once. Repeating with the two filter fields swapped guards against reading the result backwards.

**5. Edge versus diagonal LOD — confirms §3.2 and settles item 8.** Draw the same quad twice, once
with bit 8 clear and once set, under a texture matrix that makes `dudx` and `dudy` equal and
non-zero. Diagonal LOD should give exactly one level blurrier than edge LOD, since `a + a = 2a`
and `log2` of twice as much is one more. Then repeat with a matrix that makes `dudx` positive and
`dudy` negative of equal magnitude: if diagonal LOD still gives one level blurrier, the absolute
values are taken per term; if it gives the same LOD as a zero derivative, they are not.

**6. Bias clamp — settles item 4, and is the only test here that starts from a guess.** Draw a
quad at a steep grazing angle, so the two per-axis deltas differ by a large known factor, with a
LOD bias of −4.0 and bit 21 first clear and then set. With the clamp off, the bias should shift
the LOD by the full four levels. With it on, the LOD should be floored somewhere; measuring *where*
against the two known deltas tells you what the floor is a function of. Repeating at a face-on
angle, where the two deltas are equal, should suppress the bias entirely if §3.5's reading is
right. Run with edge LOD enabled, per libogc's advice, or the summed deltas will confound the
measurement.

**7. Anisotropy — settles item 5, with the most work for the least immediate return.** Draw a
ground-plane quad receding to a horizon, textured with a fine checker, at each of the three
anisotropy settings with min filter `GX_LIN_MIP_LIN` and edge LOD enabled. Dump the full frame at
each setting and difference them. The difference images show where the extra filter cycles are
spent — how far towards the horizon they begin, and how the sharpness gain scales between 2× and
4× — which is enough to reverse the probe count and spacing even without a closed-form rule. The
performance counters may help too: `GX_PERF1_TEXELS` counts texels processed and should rise with
anisotropy in proportion to the cycles actually used, which independently confirms the "only when
necessary" behaviour `gx.h:4418-4419` describes.

**8. Chain overrun — settles item 12, and should be run before any of the above are trusted.** A
32 × 32 texture with only three levels present in memory, followed by a recognisable byte pattern,
with max LOD set to 10. Sweep the minification ratio past level 3. If the recognisable pattern
appears as texels, the hardware walks off the end and the software clamp in §2.5 is the
implementation's own responsibility; if the image holds at level 3, the hardware clamps somewhere
and the clamp's rule is the next thing to find.
