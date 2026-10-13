# GameCube Video Output: the EFB→XFB Copy and the Video Interface

This document covers the last stage of the graphics pipeline: moving a finished frame out of the
embedded framebuffer (EFB) and scanning it out to a television. It is the companion to
[`gc-bp-registers.md`](gc-bp-registers.md), which documents the Blitting Processor register file
including the copy registers `0x49`–`0x54`. **Those register layouts are not repeated here.** This
document cites them and covers only what is specific to the display path, plus the entire Video
Interface (VI), which `gc-bp-registers.md` does not touch at all.

## Provenance

Three sources, all read directly:

1. **devkitPro/libogc `gx.c`** (zlib licence), the same 5199-line copy used by
   `gc-bp-registers.md` (`scratchpad/gx.c`). Line numbers below refer to that copy. This is the
   authority for everything the GX API does to the copy registers.
2. **devkitPro/libogc `video.c`** (zlib licence), fetched from
   `https://raw.githubusercontent.com/devkitPro/libogc/master/libogc/video.c` (2986 lines; line
   numbers below refer to that revision). This is the authority for the VI register layout and for
   how a video mode's timings are computed, because it programs the hardware registers directly
   with no abstraction in between.
3. **libogc headers** `D:\devkitPro\libogc\include\ogc\{video.h, video_types.h, gx.h,
   gx_struct.h, color.h}` for the mode structures, the API enumerations, and — critically for §A.2
   — the pre-converted YUV colour constants.

Unlike `gc-bp-registers.md`, **no Dolphin source was available as a cross-check for this
material**: the trimmed Dolphin headers in `roms/gc/hwtests/gxtest/` contain no video-interface or
texture-conversion code. Every claim here therefore rests on libogc alone, except §A.2, where the
conversion coefficients are *derived* and then *independently verified numerically* against
libogc's own colour table (see that section). Anything not established by those sources is marked
**Unverified:**.

Register naming: the VI registers have no names in libogc — it indexes a bare `u16` array. The
names used below (VTR, DCR, HTR0, …) are the conventional community names; the **offsets and field
positions are derived from libogc's array indices and shift expressions**, not from the naming
convention.

---

# Part A — the copy to the external framebuffer

## A.1 How a display copy differs from a texture copy

Both copies use the *same* BP registers: source rectangle `0x49`/`0x4A`, destination address
`0x4B`, destination stride `0x4D`, vertical scale `0x4E`, and the trigger `0x52`
(`gc-bp-registers.md` §9). libogc keeps two independent shadow words — `dispCopyCntrl` and
`texCopyCntrl` — that both end up in register `0x52`.

### The trigger bit

**Bit 14 of the copy trigger (`0x52`) selects the destination.** `GX_CopyDisp()` sets it and
`GX_CopyTex()` clears it, as the last thing each does before issuing the trigger:

```c
// gx.c:2040, in GX_CopyDisp()
__gx->dispCopyCntrl = (__gx->dispCopyCntrl&~0x4000)|0x4000;

// gx.c:2086, in GX_CopyTex()
__gx->texCopyCntrl = (__gx->texCopyCntrl&~0x4000);
```

Bit 14 set = **copy to the external framebuffer (XFB)**; bit 14 clear = copy to a texture in main
memory.

### What changes about the destination format

Nothing selects it. **A display copy has exactly one destination format — YUV 4:2:2 — and there is
no register field that chooses it.** The format field (bits 6:3), the intensity bit (15) and the
colour-conversion bit (16) of register `0x52` are written *only* by `GX_SetTexCopyDst()`
(`gx.c:2113-2119`); there is no `GX_SetDispCopyDst` equivalent, and `GX_SetDispCopyDst()`
(`gx.c:1996-2000`) writes nothing but the stride:

```c
void GX_SetDispCopyDst(u16 wd,u16 ht)
{
    __gx->dispCopyDst = (__gx->dispCopyDst&~0x3ff)|(_SHIFTR(wd,4,10));
    __gx->dispCopyDst = (__gx->dispCopyDst&~0xff000000)|(_SHIFTL(0x4d,24,8));
}
```

Note that `ht` is accepted and **discarded** — the destination height is implied by the source
height and the vertical scale (§A.3), not programmed.

An emulator must therefore treat bit 14 as overriding the format field entirely: when it is set,
ignore bits 6:3/15/16 and emit YUY2 (§A.2).

### What changes about the filtering

| | Display copy | Texture copy |
|---|---|---|
| 12-point sample pattern (`0x01`–`0x04`) | used | used (shared — one `GX_SetCopyFilter()` writes both) |
| 7-tap vertical filter (`0x53`, `0x54`) | used | used (shared) |
| Gamma (bits 8:7 of `0x52`) | settable via `GX_SetDispCopyGamma()` (`gx.c:1911-1914`) | **never set by libogc** — stays at `GX_GM_1_0` from `GX_Init()` |
| Vertical clamp (bits 1:0 of `0x52`) | settable via `GX_SetCopyClamp()` (`gx.c:1905-1909`) | **never set by libogc** — `GX_SetCopyClamp()` writes `dispCopyCntrl` only |
| Frame-to-field (bits 13:12) | settable via `GX_SetDispCopyFrame2Field()` (`gx.c:1979-1982`) | **never set** — stays 0 (`GX_COPY_PROGRESSIVE`); `gx.h` states this explicitly |
| Half-scale (bit 9) | **never set** — `GX_SetDispCopyDst()` does not write it | set from the `mipmap` argument of `GX_SetTexCopyDst()` (`gx.c:2118`) |
| Horizontal 2:1 chroma decimation | **always** (implied by the YUV 4:2:2 output) | only for `GX_CTF_YUVA8`/intensity formats |

The asymmetry in the first four rows is a *libogc* asymmetry, not necessarily a hardware one: the
four fields live in one register and the two shadow words are just two software copies of it. An
emulator should implement gamma, clamp and frame-to-field as functions of the register, and will
then match libogc's behaviour automatically because libogc simply leaves them at their defaults on
the texture path.

### What changes about the addressing

* **Destination address (`0x4B`)** is built identically on both paths — the physical address
  shifted right by 5, i.e. 32-byte-aligned (`gx.c:2036` and `gx.c:2078`).
* **Destination stride (`0x4D`)** is computed completely differently. For a display copy it is
  `wd >> 4` (`gx.c:1998`) — the line pitch in units of 16 pixels. For a texture copy it is
  `xtiles * zplanes` (`gx.c:2111`), a tile count.
* **The XFB is linear, 2 bytes per pixel; a copied texture is tiled.** The XFB pitch is confirmed
  twice, independently of `gx.c`:
  * `video.c:1896`, inside `__calcFbbs()`: `bytesPerLine = (wordperline<<5)&0x1fe0;` with
    `wordperline = (fbSizeX+15)/16` (`video.c:1854`). For a 640-pixel-wide XFB that is
    `40 << 5 = 1280` bytes = `640 × 2`.
  * `video_types.h:47`: `#define VI_DISPLAY_PIX_SZ 2`, used by `VIDEO_GetFrameBufferSize()`
    (`video.c:2950-2960`) as `paddedWidth * xfbHeight * 2`.

  So `0x4D`'s "16 pixels" unit is also 32 bytes for the XFB, which is why the same 32-byte
  granularity appears in both the stride and the base-address shift.

  **Unverified:** that the XFB is written in strict raster order with no tiling at all. It is
  strongly implied (a linear 2 bytes/pixel pitch is the only thing the VI's line-stride fetch in
  §B.1 can consume), but no source read here states it.

* `GX_CopyDisp()` writes the source rectangle and stride explicitly on every call
  (`gx.c:2031-2037`), so a display copy always re-arms the full register set; it does not rely on
  leftover state.

### The full `GX_CopyDisp()` register sequence

For emulator command-stream matching, `GX_CopyDisp(dest, clear)` emits, in order
(`gx.c:2010-2052`, register writes at `gx.c:2031-2043`):

1. optional `0x40` (Z mode forced to always-write) and `0x41` (blend off) when `clear` is set;
2. optional `0x43` with early-Z cleared, when clearing or when the EFB format is Z24;
3. `0x49` source top-left, `0x4A` source width/height;
4. `0x4D` destination stride;
5. `0x4B` destination address;
6. `0x52` trigger, with bit 11 = `clear` and bit 14 forced to 1;
7. restoration of `0x40`, `0x41`, `0x43` if they were overridden.

## A.2 The XFB pixel format

### Byte layout

The XFB is **YUV 4:2:2**, two horizontally adjacent pixels per 32-bit macropixel, sharing one
chroma pair. In increasing byte address order:

| Byte offset | Component |
|---|---|
| +0 | **Y0** — luma of the left (even-x) pixel |
| +1 | **Cb** — blue-difference chroma, shared by both pixels |
| +2 | **Y1** — luma of the right (odd-x) pixel |
| +3 | **Cr** — red-difference chroma, shared by both pixels |

Read as a big-endian 32-bit word (which is how the Gekko sees it), that is `0xY0_Cb_Y1_Cr`. This
is the layout usually called **YUY2 / YUYV**.

**Evidence.** `ogc/color.h` is a table of pre-converted 32-bit XFB fill values, and carries the
comment "luminance is stored twice, thus one of the lum. is redundant, but this way we can fill the
screen" — i.e. each constant is one macropixel of a solid colour. `VIDEO_ClearFrameBuffer()`
(`video.c:2962-2965`) splats such a constant across the buffer 32 bits at a time via
`__VIClearFramebuffer()` (`video.c:1753-1765`). The constants themselves fix the field order:

```c
#define COLOR_BLACK        (0x10801080)   // Y=0x10=16,  Cb=0x80=128, Y=16,  Cr=128
#define COLOR_WHITE        (0xEB80EB80)   // Y=0xEB=235, Cb=128,      Y=235, Cr=128
#define COLOR_RED          (0x515A51F0)   // Y=81, Cb=0x5A=90,  Y=81, Cr=0xF0=240
#define COLOR_BLUE         (0x29F0296E)   // Y=41, Cb=0xF0=240, Y=41, Cr=0x6E=110
```

Black and white put the *same* value in bytes 0 and 2 and a constant 128 in bytes 1 and 3, which
identifies bytes 0/2 as luma and 1/3 as chroma. Red then separates them: byte 1 is 90 (below
neutral) and byte 3 is 240 (far above), which is Cb-then-Cr, not Cr-then-Cb. Blue confirms it with
the opposite sign.

### RGB → YCbCr conversion

The converter is the standard **ITU-R BT.601 studio-swing** matrix:

```
Y  =  16 + ( 0.257·R + 0.504·G + 0.098·B)
Cb = 128 + (-0.148·R - 0.291·G + 0.439·B)
Cr = 128 + ( 0.439·R - 0.368·G - 0.071·B)
```

with R, G, B in 0..255, and the result rounded to nearest.

**Evidence.** This is not in any source read — it is hardware behaviour — so it was *derived* and
then verified numerically. Every one of the 20 `COLOR_*` constants in `ogc/color.h` was decoded to
(Y, Cb, Cr) and compared against the matrix applied to that colour's standard RGB value. **18 of
20 match to the exact byte**, with a worst error of 1 LSB on two saturated colours
(`COLOR_RED` Y: 81 stored vs 81.5 computed; `COLOR_FUCHSIA` Y: 106 vs 106.5 — both are
round-half-down rather than round-half-up cases). The two that did not match, `COLOR_MONEYGREEN`
and `COLOR_SKYBLUE`, are HTML colour names whose source RGB values could not be pinned down; they
are not evidence against the matrix.

That is strong enough to treat the coefficients and the +16/+128 offsets as established. What is
*not* established:

**Unverified:** the exact rounding mode and internal fixed-point precision of the hardware
converter. The 1-LSB discrepancies above are equally consistent with a slightly different fixed-
point coefficient set and with libogc's table having been generated by a different rounding rule
from the hardware's. An emulator that rounds to nearest will be within 1 LSB of hardware on luma.

### How the two pixels' chroma are combined

The two source pixels of a macropixel each produce a full (Y, Cb, Cr) triple; the two Y values are
kept and the two chroma pairs are reduced to one.

**Resolved 2026-09-12 against a second implementation** (this section previously guessed at an
average of the pair, and that guess was wrong).

The rule is a **three-tap filter, not a two-tap average**. The chroma sample belongs to the
macropixel's *even* pixel and reaches one pixel to either side of it:

```
Cb[pair k] = (Cb[2k-1] + 2*Cb[2k] + Cb[2k+1]) / 4      /* truncating */
Cr[pair k] = (Cr[2k-1] + 2*Cr[2k] + Cr[2k+1]) / 4
```

with the tap at `2k-1` repeating pixel `2k` at the left edge of the copied rectangle. Each pixel is
converted to (Y, Cb, Cr) first, rounding to nearest, and the filter runs on the converted chroma —
filtering the colour first and converting afterwards is a different answer by a unit or two.

**How this was settled.** libogc's constants cannot distinguish any of these rules, because every
colour in that table is solid and a solid colour filters to itself. A frame of four solid vertical
bands can: a rule is identified by what it does at the seam. Fitting the reference renderer's
colours at the first macropixel of each band back to chroma gives **exactly one part in four** of
the band before it — not one half, which is what averaging across the seam would give, and not
zero, which is what averaging within the pair gives. Three separate band boundaries agree, and the
truncating form matches to within the ±2 that the YCbCr round trip costs anyway while the rounding
form is off by three.

**Evidence layer: differential against Dolphin's software renderer — a second implementation, not
hardware.** A `hwtests`-style program that copies a hard vertical edge and reads the macropixels
back would raise this to hardware-verified, and is worth writing. Nothing in the pixel or raster
surveys covers it.

### Clamping ranges

| Component | Range |
|---|---|
| Y | 16 … 235 |
| Cb, Cr | 16 … 240 |

**Partly verified.** The endpoints are directly visible in `color.h`: `COLOR_BLACK` has Y = 16 and
`COLOR_WHITE` has Y = 235, i.e. RGB 0 and RGB 255 map exactly onto the studio-swing luma limits
rather than onto 0 and 255. `COLOR_YELLOW` (Cb = 16) and `COLOR_AQUA` (Cr = 16) hit the chroma
floor; `COLOR_BLUE` (Cb = 240) and `COLOR_RED` (Cr = 240) hit the chroma ceiling. All 20 constants
lie inside the ranges above.

**Unverified:** whether the hardware *clamps* to these ranges or whether the matrix simply cannot
exceed them for in-gamut RGB input. For 8-bit RGB input the matrix output is mathematically inside
16..235 / 16..240 anyway, so the distinction is unobservable through `GX_CopyDisp()` and only
matters if some path can feed out-of-range values into the converter.

## A.3 The vertical scale

### Ratio → register value

`GX_SetDispCopyYScale()` (`gx.c:1984-1994`):

```c
u32 GX_SetDispCopyYScale(f32 yscale)
{
    u32 ht,yScale = 0;

    yScale = ((u32)(256.0f/yscale))&0x1ff;      // gx.c:1988
    GX_LOAD_BP_REG(0x4e000000|yScale);          // gx.c:1989

    __gx->dispCopyCntrl = (__gx->dispCopyCntrl&~0x400)|(_SHIFTL(((256-yScale)>0),10,1));
    ht = _SHIFTR(__gx->dispCopyWH,12,10)+1;     // gx.c:1992
    return __GX_GetNumXfbLines(ht,yScale);
}
```

So register `0x4E` bits 8:0 (`gc-bp-registers.md` §9.3) hold **`256 / yscale`, a 1.8 fixed-point
*step*** — the number of source lines advanced per destination line, in 1/256ths.

| Register value | Meaning |
|---|---|
| 256 (`0x100`) | 1:1 — one source line per destination line |
| 128 (`0x080`) | 2× vertical **up**scale — each source line feeds two destination lines |
| 171 (`0x0AB`) | 1.5× upscale |
| > 256 | downscale (but see the range caveat below) |

The API argument `yscale` is therefore **destination height / source height**. `gx.h` documents
its range as 1.0 to 256.0, i.e. upscale or 1:1 only, which is consistent with the 9-bit field: 1.0
gives exactly 256, and anything above 1.0 gives a smaller value. A `yscale` below 1.0 would
produce a value above 511 and be silently mangled by the `&0x1ff` mask.

**A libogc inconsistency worth knowing about.** `GX_GetYScaleFactor()` (`gx.c:5167-5190`) seeds
its search with `yscale = (f32)efbHeight/(f32)xfbHeight` — the *reciprocal* of the convention its
own correction loops use two lines later (`yscale = (f32)(cnt--)/(f32)efbHeight`). The loops
converge regardless, so the returned value is correct; but the seed line cannot be used to infer
the convention. The loop body is the authority: **yscale = xfbHeight / efbHeight**.

### Trigger bit 10

`gx.c:1991` sets bit 10 of the trigger (`gc-bp-registers.md` §9.7) to `(256 - yScale) > 0`, i.e.
**vertical scaling is flagged active only when the register value is strictly below 256** — when
upscaling. A register value above 256 leaves bit 10 clear, another sign that downscale is not a
supported configuration on this path. An emulator should gate the scaler on bit 10 rather than on
the register value alone, because the two can disagree if a game writes `0x4E` directly.

### The resulting line count

`__GX_GetNumXfbLines()` (`gx.c:1059-1071`) is what turns the register value into the XFB height
the application must allocate:

```c
static u32 __GX_GetNumXfbLines(u16 efbHeight,u32 yscale)
{
    u32 tmp,tmp1;

    tmp = (((efbHeight-1)<<8)/yscale)+1;
    if(yscale>128 && yscale<256) {
        while(yscale&0x01) yscale >>= 1;
        tmp1 = yscale*(efbHeight/yscale);
        if(!(efbHeight-tmp1)) tmp++;
    }
    if(tmp>1024) tmp = 1024;

    return tmp;
}
```

The core expression is `lines = ((srcHeight - 1) × 256) / step + 1` — exactly what a stepping
scaler produces: it walks the source at `step/256` lines per output line, starting at source line
0, and the last output line is the one whose source position has not yet passed `srcHeight - 1`.
The result is capped at 1024 lines.

Worked values for a 480-line source: step 256 → 480 lines; step 128 → 959 lines; step 171 → 718
lines.

Two caveats for an emulator:

* `ht` is read from `__gx->dispCopyWH` at **bits 21:12** (`gx.c:1992`), but `GX_SetDispCopySrc()`
  wrote it at bits 19:10 via the `XY()` macro. This is the libogc bug already documented in
  `gc-bp-registers.md` §9.1. It affects only the *returned* line count, never the hardware; the
  hardware field is at 19:10.
* The `while(yscale&0x01) yscale >>= 1;` loop halves while the value is **odd**, which is almost
  certainly a transcription of "while even". It only perturbs the `+1` adjustment in the narrow
  band `128 < step < 256`. Do not replicate it; it is a software artefact and cannot affect
  hardware output.

### What the scaler does between lines

**Unverified:** the interpolation rule. The register is a step, so the scaler is walking a
fractional source position, but nothing in libogc says whether the hardware linearly interpolates
between the two straddled source lines or replicates the nearest one. What *is* certain from the
above is the source-position schedule: output line *n* samples source position `n × step / 256`.

Note that the vertical scaler is downstream of the 7-tap copy filter (§A.4), which is itself a
vertical filter over source lines — so the two vertical operations compose, and an emulator must
apply the copy filter first (in source-line space) and the scaler second.

## A.4 Gamma and the copy filter on the display path

### Gamma

Register `0x52` bits 8:7, written only by `GX_SetDispCopyGamma()` (`gx.c:1911-1914`):

```c
__gx->dispCopyCntrl = (__gx->dispCopyCntrl&~0x180)|(_SHIFTL(gamma,7,2));
```

| Value | `gx.h` constant | Gamma |
|---|---|---|
| 0 | `GX_GM_1_0` (`gx.h:941`) | 1.0 — no correction |
| 1 | `GX_GM_1_7` (`gx.h:942`) | 1.7 |
| 2 | `GX_GM_2_2` (`gx.h:943`) | 2.2 |
| 3 | — | **Unverified:** no constant; libogc never writes it |

`GX_Init()` sets `GX_GM_1_0` (`gx.c:729`).

**Difference from the texture path:** there is no `GX_SetTexCopyGamma()`. `texCopyCntrl` is a
separate shadow word that libogc never writes bits 8:7 of, so a texture copy issued by libogc
always uses gamma 1.0 regardless of what `GX_SetDispCopyGamma()` was told. The hardware field
exists in the one shared register; the restriction is libogc's.

**Unverified:** where in the pipeline gamma is applied — before or after the RGB→YCbCr conversion.
The ordering is observable on hardware (gamma on R,G,B separately is not the same as gamma on Y)
but is not determinable from these sources.

### The copy filter

`GX_SetCopyFilter(aa, sample_pattern[12][2], vf, vfilter[7])` (`gx.c:1916-1977`) writes six
registers in one call — `0x01`–`0x04` (the 12 sub-pixel sample positions,
`gc-bp-registers.md` §9.5) and `0x53`/`0x54` (the 7 vertical weights, §9.6):

```c
// gx.c:1966-1973
reg53 = 0x53000000|(vfilter[0]&0x3f);
reg53 = (reg53&~0xfc0)|(_SHIFTL(vfilter[1],6,6));
reg53 = (reg53&~0x3f000)|(_SHIFTL(vfilter[2],12,6));
reg53 = (reg53&~0xfc0000)|(_SHIFTL(vfilter[3],18,6));

reg54 = 0x54000000|(vfilter[4]&0x3f);
reg54 = (reg54&~0xfc0)|(_SHIFTL(vfilter[5],6,6));
reg54 = (reg54&~0x3f000)|(_SHIFTL(vfilter[6],12,6));
```

Each weight is 6 bits, in 1/64 units, and the seven must sum to 64 (`gx.h` states the valid range
as 0–63 and that the sum should equal 64). See §B.6 for the actual tap values and where they come
from.

The unfiltered defaults written when `vf` is false are `0x53595000` and `0x54000015`
(`gx.c:1963-1964`), decoding to weights **0, 0, 21, 22, 21, 0, 0** — a pure single-line pass-through
(the three centre taps are all "line n", see §B.6) with the flanking lines zeroed.

The sample pattern falls back to `0x01666666`, `0x02666666`, `0x03666666`, `0x04666666`
(`gx.c:1953-1956`) when `aa` is false: every component 6, the centre of the 4-bit 1/12-unit range.
`gx.h` notes that the sample pattern only takes effect when the EFB pixel format is
`GX_PF_RGB565_Z16`, i.e. when the EFB is in multisample configuration (which is also what sets
genMode bit 9 — `gc-bp-registers.md` §2).

**Differences from the texture-copy path:** none in the registers. `GX_SetCopyFilter()` is not
split into disp and tex variants — it writes the six registers directly rather than through either
shadow word, so the sample pattern and vertical filter are **global state shared by both copy
paths**. This is the one piece of copy configuration that is genuinely shared rather than
shadowed, and an emulator that models the two paths separately must not duplicate it.

The vertical clamp bits (`0x52` bits 1:0, `GX_CLAMP_TOP` = 1 / `GX_CLAMP_BOTTOM` = 2, `gx.h:931-932`)
control what the 7-tap filter does when its window runs off the top or bottom of the source
rectangle. `GX_Init()` enables both (`gx.c:727`). As noted in §A.1, `GX_SetCopyClamp()` writes only
`dispCopyCntrl`, so libogc's texture copies never clamp.

## A.5 Field mode and interlacing

Three separate mechanisms are involved, and they are easy to confuse.

### 1. `GX_SetFieldMode()` — rasterisation adjustments (BP `0x68` and `0x22`)

`gx.c:4436-4444`:

```c
void GX_SetFieldMode(u8 field_mode,u8 half_aspect_ratio)
{
    __gx->lpWidth = (__gx->lpWidth&~0x400000)|(_SHIFTL(half_aspect_ratio,22,1));
    GX_LOAD_BP_REG(__gx->lpWidth);

    __GX_FlushTextureState();
    GX_LOAD_BP_REG(0x68000000|(field_mode&1));
    __GX_FlushTextureState();
}
```

It writes **two** registers:

* **BP `0x22` bit 22** (line/point width register, `gc-bp-registers.md` §12.3) —
  `half_aspect_ratio`. Per `gx.h`, this adjusts the **line** aspect ratio to account for pixels
  being double-height on screen (pixel aspect ratio 1/2). `gx.h` notes that on retail hardware only
  lines are adjusted, not points.
* **BP `0x68` bit 0** (field mode register, `gc-bp-registers.md` §12.2) — `field_mode`. Per
  `gx.h`, this adjusts the **vertical component of the texture LOD computation**, because in
  field-mode rendering a pixel covers only half the vertical space between rendered scanlines (the
  other half being supplied by the opposite field).

Both writes are bracketed by texture-state flushes, so `0x68` appears in a command stream between
two `0x63000000` barrier writes (`gc-bp-registers.md` §10.3).

`GX_Init()` derives the two arguments from the render mode (`gx.c:719-721`):

```c
flag = 0;
if(rmode->viHeight==(rmode->xfbHeight<<1)) flag = 1;
GX_SetFieldMode(rmode->field_rendering,flag);
```

so `half_aspect_ratio` is set exactly when the XFB is half the height of the displayed picture —
i.e. for single-field (`VI_XFBMODE_SF`) modes such as `TVNtsc240Int` (xfbHeight 240, viHeight 480).
`field_mode` comes straight from the mode's `field_rendering` flag, which is `GX_TRUE` only for the
240-line *interlaced* modes (`video.c:169`, `video.c:203`) — not for the 240-line double-strike
modes and not for the 480-line modes.

Neither of these two registers affects the copy. They affect *rendering into* the EFB.

### 2. `GX_SetFieldMask()` — which EFB lines are **written** (BP `0x44`)

`gx.c:4428-4434`, layout in `gc-bp-registers.md` §12.1: bit 0 = write odd-Y pixels, bit 1 = write
even-Y pixels. `gx.h` warns that masking a field does not save fill rate — the masked field is
still computed, just not written. `GX_Init()` enables both (`gx.c:717`).

### 3. `GX_SetDispCopyFrame2Field()` — which EFB lines are **read** by the copy (BP `0x52` bits 13:12)

`gx.c:1979-1982`, and this is the one that is part of the display copy:

| Value | `gx.h` constant | Meaning |
|---|---|---|
| 0 | `GX_COPY_PROGRESSIVE` (`gx.h:952`) | read every line |
| 1 | — | **Unverified:** no constant defined; libogc never writes it |
| 2 | `GX_COPY_INTLC_EVEN` (`gx.h:953`) | read even lines only |
| 3 | `GX_COPY_INTLC_ODD` (`gx.h:954`) | read odd lines only |

`GX_Init()` sets `GX_COPY_PROGRESSIVE` (`gx.c:730`). `gx.h` states that `GX_CopyTex()` always
behaves as `GX_COPY_PROGRESSIVE`, which matches libogc never writing bits 13:12 of `texCopyCntrl`.

### How a half-height copy relates to the two fields

This is decided by the VI, not by GX, via the top-field and bottom-field base registers (§B.5) and
the `xfbMode` field of `GXRModeObj`:

* **`VI_XFBMODE_SF` (single field)** — `__calcFbbs()` leaves `bfbb == tfbb` (`video.c:1898`). Both
  fields scan out the *same* buffer. A 240-line EFB copied 1:1 into a 240-line XFB is displayed
  twice per frame, once per field: **double-strike / 240p**. This is `TVNtsc240Ds`
  (`video.c:90-122`) and also every progressive mode (`TVNtsc480Prog`, `video.c:329-361`, uses SF
  with a 480-line XFB because progressive scan-out has no second field).
* **`VI_XFBMODE_DF` (double field)** — `__calcFbbs()` sets `*bfbb = *tfbb + bytesPerLine`
  (`video.c:1899`). The second field starts one line lower, so the two fields interleave alternate
  lines of one full-height buffer. A 480-line EFB copied 1:1 into a 480-line XFB is scanned out as
  two 240-line fields: **true interlace**. This is `TVNtsc480IntDf` (`video.c:260-292`).
* A 240-line EFB can also feed a 480-line DF XFB by setting the vertical scale to 2× upscale
  (§A.3) — a 240-line render displayed as a full-height interlaced picture.
* If `dispPosY` is odd, `__calcFbbs()` **swaps** the two base addresses (`video.c:1901-1905`), so
  the field parity flips. `VIDEO_GetNextField()` compensates for exactly this
  (`video.c:2808`): `return nextfield^(HorVer.adjustedDispPosY&0x0001);`

Finally, `TVNtsc480IntAa` (`video.c:294-326`) shows the AA case: `efbHeight` is **242**, not 240 —
two extra lines above and below so the 7-tap vertical filter has real data at the field edges —
while `xfbHeight` stays 480. `VIDEO_GetFrameBufferSize()` correspondingly adds 4 lines to the
allocation when `rmode->aa` is set (`video.c:2956-2957`).

---

# Part B — the Video Interface

## B.1 VI register map

The VI register block starts at **`0xCC002000`**. libogc addresses it as a `volatile u16` array
(`video.c:1626`):

```c
static vu16* const _viReg = (u16*)0xCC002000;
```

and uses 60 entries (`static u16 regs[60];`, `video.c:1608`), so the block is
**`0xCC002000`–`0xCC002077`, 120 bytes**. Index *n* is at byte offset *2n*. Because the Gekko is
big-endian, a 32-bit register at offset *2n* has `_viReg[n]` as its **high** half-word (bits 31:16)
and `_viReg[n+1]` as its **low** half-word (bits 15:0). All the multi-half-word field positions
below are derived that way.

| Offset | Size | `_viReg` idx | Name | Fields | Evidence |
|---|---|---|---|---|---|
| `0x00` | 16 | 0 | **VTR** — vertical timing | 3:0 EQU (equalisation pulse, half-lines); 13:4 ACV (active video lines **per field**) | `regs[0] = ((tmp<<4)&~0x0f)\|equ;` — `video.c:2006` |
| `0x02` | 16 | 1 | **DCR** — display configuration | 0 enable; 1 reset; 2 non-interlace (also set for progressive); 3 3D mode; 9:8 video format (0 NTSC, 1 PAL, 2 MPAL, 3 debug) | `video.c:2098, 2105, 2137, 2140, 2574-2575, 2677-2681, 2983` |
| `0x04` | 32 | 2, 3 | **HTR0** — horizontal timing 0 | 8:0 HLW (half-line width, VI clocks); 22:16 HCE (colour-burst end); 30:24 HCS (colour-burst start) | `regs[2] = (tm->hcs<<8)\|tm->hce; regs[3] = tm->hlw;` — `video.c:1955-1956` |
| `0x08` | 32 | 4, 5 | **HTR1** — horizontal timing 1 | 6:0 HSY (h-sync width); 16:7 HBE640 (h-blank end); 27:17 HBS640 (h-blank start) | `regs[4] = (val1>>9)\|(val2<<1); regs[5] = (val1<<7)\|tm->hsy;` — `video.c:1962-1963` |
| `0x0C` | 32 | 6, 7 | **VTO** — odd-field vertical timing | 9:0 PRB (pre-blanking, half-lines); 25:16 PSB (post-blanking, half-lines) | `regs[6] = psbodd; regs[7] = prbodd;` — `video.c:2009-2010` |
| `0x10` | 32 | 8, 9 | **VTE** — even-field vertical timing | same layout as VTO | `regs[8] = psbeven; regs[9] = prbeven;` — `video.c:2014-2015` |
| `0x14` | 32 | 10, 11 | **BBEI** — burst blanking, odd field | 4:0 BS1; 15:5 BE1; 20:16 BS3; 31:21 BE3 | `regs[10] = (be3<<5)\|bs3; regs[11] = (be1<<5)\|bs1;` — `video.c:1866-1867` |
| `0x18` | 32 | 12, 13 | **BBOI** — burst blanking, even field | 4:0 BS2; 15:5 BE2; 20:16 BS4; 31:21 BE4 | `regs[12] = (be4<<5)\|bs4; regs[13] = (be2<<5)\|bs2;` — `video.c:1871-1872` |
| `0x1C` | 32 | 14, 15 | **TFBL** — top field base, left | 23:0 address; 27:24 XOF (horizontal offset in pixels); 28 page-offset (see §B.2) | `video.c:1928-1929` |
| `0x20` | 32 | 16, 17 | **TFBR** — top field base, right (3D) | 23:0 address only | `video.c:1939-1940` |
| `0x24` | 32 | 18, 19 | **BFBL** — bottom field base, left | 23:0 address only | `video.c:1933-1934` |
| `0x28` | 32 | 20, 21 | **BFBR** — bottom field base, right (3D) | 23:0 address only | `video.c:1944-1945` |
| `0x2C` | 16 | 22 | **DPV** — current vertical position | 10:0 line counter, **1-based** | `vpos = (_viReg[22]&0x7ff);` — `video.c:2326` |
| `0x2E` | 16 | 23 | **DPH** — current horizontal position | 10:0 pixel counter, **1-based** | `hpos = (_viReg[23]&0x7ff);` — `video.c:2329` |
| `0x30` | 32 | 24, 25 | **DI0** — display interrupt 0 | 9:0 HCT (horizontal position); 25:16 VCT (vertical position); 28 enable; 31 status (write 0 to acknowledge) | `regs[24] = 0x1000\|((tm->nhlines/2)+1); regs[25] = hlw+1;` — `video.c:1846-1847`; ack at `video.c:2471-2473` |
| `0x34` | 32 | 26, 27 | **DI1** | same layout | `_viReg[26] = 0x1001; _viReg[27] = 0x0001;` — `video.c:2131-2132` |
| `0x38` | 32 | 28, 29 | **DI2** | same layout | acknowledged at `video.c:2483-2487`; **never programmed by libogc** |
| `0x3C` | 32 | 30, 31 | **DI3** | same layout | acknowledged at `video.c:2489-2493`; **never programmed by libogc** |
| `0x40` | 32 | 32, 33 | **DL0** — display latch 0 | **Unverified:** never touched by libogc |
| `0x44` | 32 | 34, 35 | **DL1** — display latch 1 | **Unverified:** never touched by libogc |
| `0x48` | 16 | 36 | **HSW** — picture configuration | 7:0 STD (memory stride per displayed line, in 32-byte words); 14:8 WPL (32-byte words fetched per line) | `regs[36] = (*wpl<<8)\|*std;` — `video.c:1860` |
| `0x4A` | 16 | 37 | **HSR** — horizontal scale | 8:0 STP (step, 1.8 fixed point; `0x100` = 1:1); 12 horizontal-scaling enable | `regs[37] = 0x1000\|((dispSizeX+(_SHIFTL(panSizeX,8,16)-1))/dispSizeX);` else `0x100` — `video.c:1881-1886` |
| `0x4C` | 32 | 38, 39 | **FCT0** — filter coefficients | 9:0 tap0; 19:10 tap1; 29:20 tap2 | `video.c:2556-2557` |
| `0x50` | 32 | 40, 41 | **FCT1** | 9:0 tap3; 19:10 tap4; 29:20 tap5 | `video.c:2558-2559` |
| `0x54` | 32 | 42, 43 | **FCT2** | 9:0 tap6; 19:10 tap7; 29:20 tap8 | `video.c:2560-2561` |
| `0x58` | 32 | 44, 45 | **FCT3** | 7:0 tap9; 15:8 tap10; 23:16 tap11; 31:24 tap12 | `video.c:2562-2563` |
| `0x5C` | 32 | 46, 47 | **FCT4** | 7:0 tap13; 15:8 tap14; 23:16 tap15; 31:24 tap16 | `video.c:2564-2565` |
| `0x60` | 32 | 48, 49 | **FCT5** | 7:0 tap17; 15:8 tap18; 23:16 tap19; 31:24 tap20 | `video.c:2566-2567` |
| `0x64` | 32 | 50, 51 | **FCT6** | 7:0 tap21; 15:8 tap22; 23:16 tap23; 31:24 tap24 | `video.c:2568-2569` |
| `0x68` | 32 | 52, 53 | — | **Unverified:** never touched by libogc |
| `0x6C` | 16 | 54 | **VICLK** — clock select | 0: 0 = 27 MHz (interlace/double-strike), 1 = 54 MHz (progressive) | `_viReg[54] = 0x0001;` for progressive, `0x0000` otherwise — `video.c:2138, 2141, 2684-2685`; read back with the comment "Check Clock Select Register for progressive clock" — `video.c:2979-2980` |
| `0x6E` | 16 | 55 | **VISEL** — DTV status | 0: component cable present | `return (_viReg[55]&0x01);` in `VIDEO_HaveComponentCable()` — `video.c:2969` |
| `0x70` | 16 | 56 | **FBWIDTH** — scaler source width | source pixels per line fed to the horizontal scaler | `regs[56] = panSizeX;` — `video.c:1882`; `_viReg[56] = 640;` — `video.c:2570` |
| `0x72` | 16 | 57 | — | **Unverified:** never touched by libogc |
| `0x74` | 32 | 58, 59 | — | **Unverified:** never touched by libogc |

Two structural notes an emulator must model:

* **Writes are deferred.** `VIDEO_Flush()` (`video.c:2763-2786`) copies the dirty registers into a
  shadow set and raises `flushFlag`; the retrace handler then calls `__VISetRegs()`
  (`video.c:2360-2381`) which walks the dirty bitmask and performs the real MMIO writes. So a
  configuration change takes effect at a field boundary, not when the game writes it. Worse, if the
  interlace *mode* changed, `__VISetRegs()` refuses to apply until the current field is the odd one
  (`if(shdw_changeMode==1){ if(!__getCurrentFieldEvenOdd()) return 0; }`). `VIDEO_SetFramebuffer()`
  (`video.c:2715-2737`) is the exception: it writes TFBL/BFBL immediately.
* **DPV/DPH are 1-based.** `__getCurrentHalfLine()` decrements both before using them
  (`video.c:2343-2344`).

## B.2 How a framebuffer address register encodes an address

From `__setFbbRegs()` (`video.c:1911-1949`):

```c
flag = 1;
if((*tfbb)<0x01000000 && (*bfbb)<0x01000000
    && (*rtfbb)<0x01000000 && (*rbfbb)<0x01000000) flag = 0;

if(flag) {
    *tfbb >>= 5;
    *bfbb >>= 5;
    *rtfbb >>= 5;
    *rbfbb >>= 5;
}

regs[14] = _SHIFTL(flag,12,1)|_SHIFTL(horVer->xof,8,4)|_SHIFTR(*tfbb,16,8);
regs[15] = *tfbb&0xffff;
```

Reassembling the 32-bit register at `0x1C`:

| Bits | Field |
|---|---|
| 23:0 | Address — **either the raw physical byte address, or the physical address shifted right by 5 (32-byte units)**, depending on bit 28 |
| 27:24 | XOF — horizontal pixel offset within the first 16-pixel word, `panPosX % 16` (`video.c:1858`) |
| 28 | **Page-offset bit** — 0 = bits 23:0 are a byte address; 1 = bits 23:0 are the address in 32-byte units |
| 31:29 | **Unverified:** not written |

So the register reaches the whole of MEM1 (24 MB) as a plain byte address with bit 28 clear, and
with bit 28 set the same 24 bits cover 512 MB in 32-byte granules — which is how a Wii XFB in MEM2
(`0x10000000`+) is addressed. libogc chooses the mode with a single threshold test: if **all four**
base addresses are below `0x01000000` (16 MB) it uses byte addressing, otherwise all four switch to
shifted addressing. An emulator must decode bit 28 per register write, not assume one convention.

The address itself is computed by `__calcFbbs()` (`video.c:1891-1909`):

```c
panPosX &= 0xfff0;
bytesPerLine = (wordperline<<5)&0x1fe0;
*tfbb = bufAddr+((panPosX<<5)+(panPosY*bytesPerLine));
*bfbb = *tfbb;
if(xfbMode==VI_XFBMODE_DF) *bfbb = *tfbb+bytesPerLine;
```

then converted to physical with `MEM_VIRTUAL_TO_PHYSICAL`. Note the `panPosX & 0xfff0` — the pan
position is quantised to 16 pixels and the sub-word remainder is carried separately in the XOF
field. `VIDEO_SetNextFramebuffer()` warns (in the debug build) if the address is not 32-byte
aligned (`video.c:2743`).

Only **TFBL** carries XOF and the page-offset bit. TFBR, BFBL and BFBR receive only the 24-bit
address split across two half-words (`video.c:1933-1934, 1939-1940, 1944-1945`), so those three
registers inherit TFBL's addressing convention.

## B.3 NTSC interlaced 640×480 — the worked example

### The mode and the timing table

`TVNtsc480IntDf` (`video.c:260-292`): `viTVMode = VI_TVMODE_NTSC_INT`, `fbWidth = 640`,
`efbHeight = 480`, `xfbHeight = 480`, `viXOrigin = (720-640)/2 = 40`, `viYOrigin = (480-480)/2 = 0`,
`viWidth = 640`, `viHeight = 480`, `xfbMode = VI_XFBMODE_DF`, `field_rendering = GX_FALSE`,
`aa = GX_FALSE`.

`__gettiming(VI_TVMODE_NTSC_INT)` returns `video_timing[0]` (`video.c:1718-1721`), which is
(`video.c:1525-1533`):

| Field | Value | Meaning |
|---|---|---|
| `equ` | 6 | equalisation pulses, **half-lines** |
| `acv` | 240 (`0x00F0`) | active video **lines per field** |
| `prbOdd` / `prbEven` | 24 / 25 (`0x18`/`0x19`) | pre-blanking, **half-lines** |
| `psbOdd` / `psbEven` | 3 / 2 | post-blanking, **half-lines** |
| `bs1`/`bs2`/`bs3`/`bs4` | 12 / 13 / 12 / 13 | burst blanking start, **half-lines** |
| `be1`/`be2`/`be3`/`be4` | 520 / 519 / 520 / 519 | burst blanking end, **half-lines** |
| `nhlines` | 525 (`0x020D`) | **half-lines per field** |
| `hlw` | 429 (`0x01AD`) | half-line width, **VI clocks** |
| `hsy` | 64 (`0x40`) | h-sync width, VI clocks |
| `hcs` / `hce` | 71 / 105 (`0x47`/`0x69`) | colour-burst start/end, VI clocks |
| `hbe640` | 162 (`0xA2`) | h-blank end, VI clocks |
| `hbs640` | 373 (`0x0175`) | h-blank start, VI clocks |

Sanity check on the units: `hlw × 2 = 858` VI clocks per line, `858 × 525 = 450450` clocks per
frame, `× 59.94 Hz ≈ 27.0 MHz` — the VI clock. So a *half-line* is 429 clocks of 27 MHz, and
`nhlines = 525` half-lines per field is 262.5 lines per field, 525 lines per frame. The VI's whole
vertical timing vocabulary is in half-lines; only `acv` (and the ACV register field) is in whole
lines.

### What `VIDEO_Configure(&TVNtsc480IntDf)` computes

Intermediate values, following `video.c:2625-2699` with `displayOffsetH = displayOffsetV = 0`:

```
nonInter        = 0  (VI_INTERLACE)
tv              = 0  (VI_NTSC)
dispPosX = 40   dispPosY = 0    dispSizeX = 640
fbSizeX  = 640  fbSizeY  = 480  fbMode = VI_XFBMODE_DF
panPosX  = 0    panPosY  = 0    panSizeX = 640   panSizeY = 480
dispSizeY = panSizeY = 480      (DF branch, video.c:2659)
threeD    = 0
```

`__adjustPosition(240)` (`video.c:2020-2063`) then yields
`adjustedDispPosX = 40`, `adjustedDispPosY = 0`, `adjustedDispSizeY = 480`,
`adjustedPanPosY = 0`, `adjustedPanSizeY = 480`.

### The registers libogc programs

| Offset | Value | Derivation | Meaning |
|---|---|---|---|
| `0x00` VTR | `0x0F06` | `((240<<4)&~0x0f)\|6` — `video.c:2006`, with `tmp = dispSizeY/div1 = 480/2` | EQU = 6 half-lines; ACV = 240 lines per field |
| `0x02` DCR | `0x0001` | `dcr = regs[1]&~0x030c` then no bits set (interlace, NTSC, no 3D) — `video.c:2676-2681` | enabled, interlaced, NTSC |
| `0x04` HTR0 | `0x4769_01AD` | `regs[2] = (0x47<<8)\|0x69`, `regs[3] = 0x1AD` — `video.c:1955-1956` | HLW = 429 clocks; HCE = 105; HCS = 71 |
| `0x08` HTR1 | `0x02EA_5140` | `val1 = (162+40-40)&0x1ff = 162`; `val2 = (373+40+40)-(720-640) = 373`; `regs[4] = (162>>9)\|(373<<1) = 0x02EA`; `regs[5] = (162<<7)\|64 = 0x5140` — `video.c:1960-1963` | HSY = 64; HBE640 = 162; HBS640 = 373 clocks |
| `0x0C` VTO | `0x0003_0018` | `psbodd = psbOdd + 0 = 3`; `prbodd = prbOdd + 0 = 24` — `video.c:1983-1992, 2009-2010` | odd field: 24 half-lines pre-blank, 3 half-lines post-blank |
| `0x10` VTE | `0x0002_0019` | `psbeven = 2`; `prbeven = 25` — `video.c:2014-2015` | even field: 25 / 2 half-lines |
| `0x14` BBEI | `0x410C_410C` | `regs[10] = (520<<5)\|12`; `regs[11] = (520<<5)\|12` — `video.c:1866-1867` | BS1 = BS3 = 12, BE1 = BE3 = 520 half-lines |
| `0x18` BBOI | `0x40ED_40ED` | `regs[12] = (519<<5)\|13`; `regs[13] = (519<<5)\|13` — `video.c:1871-1872` | BS2 = BS4 = 13, BE2 = BE4 = 519 half-lines |
| `0x1C` TFBL | `flag:0 XOF:0 addr:XFB` | `video.c:1928-1929` | see §B.2 and §B.5 |
| `0x24` BFBL | `addr:XFB + 1280` | `bytesPerLine = 40<<5 = 1280`, DF branch — `video.c:1896-1899` | second field starts one line lower |
| `0x30` DI0 | `0x1107_01AE` | `regs[24] = 0x1000\|((525/2)+1) = 0x1000\|263`; `nhlines` is odd so `hlw = 429` and `regs[25] = 430` — `video.c:1844-1847` | enable + VCT = 263, HCT = 430 |
| `0x34` DI1 | `0x1001_0001` | `_viReg[26] = 0x1001; _viReg[27] = 0x0001;` — `video.c:2131-2132` | enable + VCT = 1, HCT = 1 |
| `0x48` HSW | `0x2850` | `wordPerLine = (640+15)/16 = 40`; `std = 40<<1 = 80` (DF); `xof = 0`; `wpl = (0+655)/16 = 40`; `regs[36] = (40<<8)\|80` — `video.c:1854-1860` | fetch 40 words (1280 bytes) per displayed line; advance 80 words (2560 bytes) between lines of the same field |
| `0x4A` HSR | `0x0100` | `panSizeX (640) < dispSizeX (640)` is false → `regs[37] = 0x100` — `video.c:1886` | horizontal scaler disabled, 1:1 |
| `0x6C` VICLK | `0x0000` | `regs[54] &= ~0x0001` — `video.c:2684-2685` | 27 MHz (non-progressive) |
| `0x70` FBWIDTH | `640` | `_viReg[56] = 640` — `video.c:2570`; untouched by `__setScalingRegs` in this mode | 640 source pixels per line |

The DF stride is the key line to get right: **`std` is doubled for `VI_XFBMODE_DF`**
(`video.c:1856`, `if(xfbMode==VI_XFBMODE_DF) *std <<= 1;`), because each field skips the other
field's lines. `wpl` is *not* doubled — the VI still fetches only 640 pixels of data per displayed
line.

### The "blank" variant

`VIDEO_Init()` sets `HorVer.black = 1` (`video.c:2602`), and `__setVerticalRegs()` handles that by
collapsing the active region into the blanking intervals (`video.c:1998-2004`):

```c
tmp = dispSizeY/div1;
if(black) {
    prbodd += ((tmp<<1)-2);
    prbeven += ((tmp<<1)-2);
    psbodd += 2;
    psbeven += 2;
    tmp = 0;
}
```

For this mode that gives VTR = `0x0006` (ACV = 0), VTO = `0x0005_01F6` (psb 5, prb 502) and
VTE = `0x0004_01F7` (psb 4, prb 503) — 502 = 24 + 478 and 478 = (240 × 2) − 2. Those are exactly
the values `__VIInit()` programs directly at boot (`video.c:2116-2120`):

```c
_viReg[6] = (cur_timing->psbOdd+2);                          // 5
_viReg[7] = (cur_timing->prbOdd+((cur_timing->acv<<1)-2));   // 502
```

so the hardware comes up displaying nothing until the application calls `VIDEO_SetBlack(FALSE)`.
An emulator that boots straight into a game will see this transition and must not mistake the
initial ACV = 0 for a broken mode.

## B.4 When the four display interrupts fire

A DI register (§B.1) fires when the display position reaches (VCT, HCT), where both are 1-based
and VCT counts *lines* while HCT counts *VI clocks within the line*. Converting to the half-line
index that libogc itself uses (`__getCurrentHalfLine()`, `video.c:2336-2348`):

```c
hpos--;
vpos--;
vpos <<= 1;
return vpos+(hpos/currTiming->hlw);
```

so `halfline = (VCT − 1) × 2 + ((HCT − 1) / hlw)`, and a field boundary is at
`halfline == nhlines` (`__getCurrentFieldEvenOdd()`, `video.c:2350-2358`).

For NTSC interlaced 640×480 (`nhlines = 525`, `hlw = 429`):

| Interrupt | Programmed | Half-line | Fires at |
|---|---|---|---|
| **DI0** (`0x30`) | VCT = 263, HCT = 430, enabled | `(263−1)×2 + (429/429) = 524 + 1 = 525` | exactly `nhlines` — the **first half-line of the second (even) field** |
| **DI1** (`0x34`) | VCT = 1, HCT = 1, enabled | `(1−1)×2 + (0/429) = 0` | half-line 0 — the **first half-line of the first (odd) field** |
| **DI2** (`0x38`) | **never programmed** | — | available for the application |
| **DI3** (`0x3C`) | **never programmed** | — | available for the application |

**libogc uses DI0 and DI1 together as the retrace.** Both are enabled at init; between them they
fire once per field, i.e. 59.94 times a second for NTSC. All four share one Processor Interface
IRQ line (`IRQ_Request(IRQ_PI_VI,__VIRetraceHandler,NULL);`, `video.c:2617`), and
`__VIRetraceHandler()` (`video.c:2462-2529`) polls all four status bits, acknowledges whichever are
set, then increments `retraceCount` **once per invocation regardless of which fired**. So:

* `retraceCount` counts **fields**, not frames. `VIDEO_WaitVSync()` waits for it to change
  (`video.c:2702-2713`), so a `VIDEO_WaitVSync()` in an NTSC interlaced mode returns at 59.94 Hz.
* The deferred register flush (`__VISetRegs()`, §B.1) happens inside this handler
  (`video.c:2507-2512`), so VI state changes land on a field boundary.
* DI2 and DI3 are what the position callback is for: `if(ret&0x04 || ret&0x08)` — i.e. only if DI2
  or DI3 fired — the handler computes the current raster position and calls `positionCB`
  (`video.c:2496-2501`). Since libogc never programs DI2/DI3 and exposes no setter for `positionCB`
  (it is a file-static initialised to `NULL`, `video.c:1624`), this path is dead in stock libogc but
  is exactly what a game would use for a mid-frame raster effect.

The acknowledge protocol is: read the high half-word, test bit 15 (bit 31 of the 32-bit register),
and write it back with that bit cleared (`video.c:2471-2493`). An emulator must make the status bit
sticky until written clear, or the handler will spin.

For reference, the progressive NTSC mode (`video_timing[6]`, `video.c:1579-1587`: `nhlines = 1050`,
`hlw = 429`) gives DI0 = `0x120E_0001` — VCT = 526, HCT = 1, because `nhlines` is even so
`__setInterruptRegs()` leaves `hlw = 0` and writes `regs[25] = 1` (`video.c:1844-1847`). Half-line
`(526−1)×2 + 0 = 1050 = nhlines`: the same rule, one interrupt at each half of the 1050-half-line
frame, so `retraceCount` still advances twice per frame.

## B.5 How the interlaced fields are scanned out

The VI holds **two** base addresses per eye:

* **TFBL** (`0x1C`) — the *top* field, i.e. the field whose first line is the topmost line of the
  picture.
* **BFBL** (`0x24`) — the *bottom* field.

(**TFBR** `0x20` and **BFBR** `0x28` are the right-eye equivalents, written only when
`HorVer.threeD` is set — `video.c:1938-1948` — which happens only for
`VI_NON_INTERLACE|VI_PROGRESSIVE`, `video.c:2661`.)

`__calcFbbs()` (`video.c:1891-1909`) decides the relationship:

| `xfbMode` | Result | Scan-out |
|---|---|---|
| `VI_XFBMODE_DF` | `bfbb = tfbb + bytesPerLine` | The two fields read **interleaved alternate lines** of one full-height buffer. Combined with the doubled `std` in HSW (§B.3), field 1 reads lines 0, 2, 4 … and field 2 reads lines 1, 3, 5 …  This is true interlace. |
| `VI_XFBMODE_SF` | `bfbb = tfbb` | Both fields read **the same lines** of a half-height buffer. Double-strike (240p): every line is drawn in both fields. |

Then, if the vertical display origin is odd:

```c
if(dispPosY%2) {
    tmp = *tfbb;
    *tfbb = *bfbb;
    *bfbb = tmp;
}
```

— the two bases are **swapped**, flipping which field gets which set of lines.
`VIDEO_GetNextField()` undoes this in software so the caller still sees the true parity
(`video.c:2808`).

**How a progressive mode differs.** Three things change, none of which is the base-address pair:

1. **DCR bit 2 is set** and **VICLK (`0x6C`) is set to 1**, selecting the 54 MHz pixel clock
   (`video.c:2137-2138`, `video.c:2678`, `video.c:2685`). `VIDEO_GetVideoScanMode()`
   (`video.c:2972-2986`) reads VICLK bit 0 *first* precisely because DCR bit 2 alone cannot
   distinguish progressive from double-strike — both set it.
2. **The vertical timing switches to whole-frame units.** `__setVerticalRegs()` branches on
   `equ >= 10` (`video.c:1978-1981`):
   ```c
   div1 = 2; div2 = 1;
   if(equ>=10) { div1 = 1; div2 = 2; }
   ```
   The progressive timing table has `equ = 12` (`video.c:1580`) versus 6 for interlaced, so
   `div1 = 1` and the ACV field receives the **full** 480 lines instead of 240 per field, while
   the pre/post-blanking half-line counts are doubled. For `TVNtsc480Prog` this gives
   VTR = `0x1E0C` (ACV = 480, EQU = 12), VTO = VTE = `0x0006_0030` (psb 6, prb 48 half-lines).
3. **`xfbMode` is `VI_XFBMODE_SF` with a full-height XFB.** `TVNtsc480Prog` (`video.c:329-361`)
   has `xfbHeight = 480` and `xfbMode = VI_XFBMODE_SF`, so `bfbb == tfbb` and `std` is *not*
   doubled — every line is fetched consecutively in one pass. `VIDEO_Configure()` takes the
   progressive branch for `dispSizeY` (`video.c:2657`), so `dispSizeY = panSizeY = 480` rather
   than `panSizeY << 1`.

So a progressive mode still writes both base registers, but they hold the same address and the
stride is single, which is what turns two half-rate field passes into one full-rate frame pass.

## B.6 The anti-flicker filter's seven taps

The anti-flicker (deflicker) filter is the **7-tap vertical filter applied during the EFB→XFB
copy** — BP registers `0x53`/`0x54` (`gc-bp-registers.md` §9.6), programmed by
`GX_SetCopyFilter()` (§A.4). It is not a VI feature; it runs once, at copy time, and what reaches
the VI is already filtered. This is the correct place to implement it in an emulator: at the copy,
in EFB-line space, *before* the vertical scaler (§A.3).

### Where the taps come from

They come from the `vfilter[7]` array embedded in each `GXRModeObj`, in `video.c`. The seven taps
straddle three source lines, in the layout the comments in `video.c` spell out for every mode:

| Tap | Source line |
|---|---|
| 0 | line n−1 |
| 1 | line n−1 |
| 2 | line n |
| 3 | line n |
| 4 | line n |
| 5 | line n+1 |
| 6 | line n+1 |

Each is 6 bits in 1/64 units and the seven must sum to 64 — `gx.h` states the valid range as 0–63
and the required sum as 64. The two taps per neighbouring line and three per centre line
correspond to the sub-line sample positions the EFB's multisample grid provides.

### The tap sets libogc ships

| Mode family | Taps 0..6 | Sum | Effect |
|---|---|---|---|
| Non-deflickered (e.g. `TVNtsc480Int`, `video.c:249-257`; `TVNtsc240Ds`, `video.c:113-121`; `TVNtsc480Prog`, `video.c:352-360`) | 0, 0, **21, 22, 21**, 0, 0 | 64 | No vertical blending at all — the centre line only. Identical to the hardcoded default `GX_SetCopyFilter()` writes when `vf` is false (`0x53595000` / `0x54000015`, `gx.c:1963-1964`). |
| Deflickered (`TVNtsc480IntDf`, `video.c:283-291`; `TVNtsc480ProgSoft`, `video.c:386-394`) | **8, 8**, 10, 12, 10, **8, 8** | 64 | 16/64 (25%) of the weight comes from each neighbouring line. This is the standard interlace anti-flicker setting and is what `VIDEO_GetPreferredMode()` selects for a non-progressive NTSC console (`TVNtsc480IntDf`, `video.c:2856`). |
| Anti-aliased (`TVNtsc480IntAa`, `video.c:317-325`; `TVNtsc480ProgAa`, `video.c:420-428`) | **4, 8**, 12, 16, 12, **8, 4** | 64 | A tent/triangle kernel — 12/64 from each neighbour, weighted toward the nearer sub-line sample. Paired with the non-centred 12-point sample pattern `{3,2},{9,6},{3,10},…` and with `efbHeight` reduced to 242 so the kernel has data at the field edges. |
| 240-line AA (`TVNtsc240IntAa`, `video.c:215-223`) | 0, 0, **21, 22, 21**, 0, 0 | 64 | AA modes at 240 lines still use the flat filter — the anti-aliasing there comes from the sample pattern alone, not the vertical filter. |

The application is expected to pass them straight through:

```c
GXRModeObj* rmode = VIDEO_GetPreferredMode(NULL);
GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
```

(this exact idiom is in `gx.h`'s documentation for `GX_SetCopyFilter()`).

### Not to be confused with: the VI's own 26-entry filter table

The VI has a *separate* coefficient table in registers FCT0–FCT6 (`0x4C`–`0x67`, §B.1), loaded once
by `VIDEO_Init()` from a fixed table and never changed (`video.c:1506-1512, 2556-2569`):

```c
static const u16 taps[26] = {
    0x01F0,0x01DC,0x01AE,0x0174,0x0129,0x00DB,
    0x008E,0x0046,0x000C,0x00E2,0x00CB,0x00C0,
    0x00C4,0x00CF,0x00DE,0x00EC,0x00FC,0x0008,
    0x000F,0x0013,0x0013,0x000F,0x000C,0x0008,
    0x0001,0x0000
};
```

The first nine entries are 10 bits wide and packed three to a register (FCT0–FCT2); entries 9–24
are 8 bits wide and packed four to a register (FCT3–FCT6). Entry 25 is unused. This is a
**horizontal** reconstruction/resampling kernel in the scan-out path, not the deflicker filter —
libogc writes it once with a constant and never varies it by mode, so it is mode-independent
hardware setup.

**Unverified:** the exact function of the 26-entry table (the split into a 9-entry 10-bit group and
a 16-entry 8-bit group, and what each group filters). libogc treats it as an opaque magic constant
with no comment. An emulator can reproduce libogc's behaviour exactly by writing these values and
otherwise ignoring them.

---

# What could not be determined

Recorded as **Unverified:** above, in descending order of how much it matters for getting a picture
on screen:

1. **The chroma-combining rule for the 4:2:2 macropixel** (§A.2). Averaging is the only sensible
   answer and is what should be implemented, but libogc's solid-colour constants cannot distinguish
   averaging from dropping the odd pixel's chroma. Needs a hardware test with a 1-pixel-wide
   vertical colour edge.
2. **The vertical scaler's interpolation rule** (§A.3). The source-position schedule is certain
   (`n × step / 256`); whether the hardware blends the two straddled lines or replicates the
   nearest one is not.
3. **Where gamma sits in the pipeline** (§A.4) — before or after the RGB→YCbCr conversion.
4. **The exact rounding and internal precision of the RGB→YCbCr converter** (§A.2). The
   coefficients and offsets are established to within 1 LSB against 18 independent colour samples;
   the rounding mode is not.
5. **Whether YCbCr output is actively clamped** to 16..235 / 16..240 (§A.2), or whether the ranges
   are simply the reachable output of the matrix for in-gamut input.
6. **The function of the VI's 26-entry filter coefficient table** (`0x4C`–`0x67`, §B.6). libogc
   writes a fixed magic constant with no explanation. Reproducing the constant is sufficient to
   match libogc.
7. **VI registers never touched by libogc** (§B.1): `0x40`/`0x44` (the display latches), `0x68`,
   `0x72`, `0x74`. No evidence of any kind from these sources.
8. **Copy trigger (`0x52`) frame-to-field value 1** (§A.5) and **gamma value 3** (§A.4). Both are
   encodable but have no `gx.h` constant and libogc never writes them.
9. **Whether the XFB is written in strict raster order** (§A.1). Strongly implied by the VI's
   linear line-stride fetch and the 2-bytes-per-pixel pitch, but not stated anywhere.
10. **Bits 31:29 of the framebuffer base registers** (§B.2). Never written.

Two libogc source-level quirks documented above must **not** be reproduced in an emulator, because
they are software artefacts that never reach hardware: the `_SHIFTR(dispCopyWH,12,10)` height
read-back in `GX_SetDispCopyYScale()` (`gx.c:1992`, already noted in `gc-bp-registers.md` §9.1),
and the inverted `while(yscale&0x01)` loop in `__GX_GetNumXfbLines()` (`gx.c:1065`).
