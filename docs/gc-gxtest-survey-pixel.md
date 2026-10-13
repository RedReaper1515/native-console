# GX hardware-test survey: the four pixel-pipeline tests

Survey of `roms/gc/hwtests/gxtest/{tev,lighting,copyfilter,intensity}.cpp` and the shared
harness (`util.cpp/.h`, `cgx.cpp/.h`, `cgx_defaults.h`, with register layouts from
`BPMemory.h`, `XFMemory.h`, `CPMemory.h`).

Source is Dolphin's hwtests suite, GPL-2.0. **This document describes behaviour only. No
code was copied; formulas are restated in prose/notation. Do not paste the upstream C++
into our tree.** Literal magic constants (coefficients, rounding biases, the konst table)
are facts about the GameCube GPU, not expression of the test program, and are reproduced
here because an implementation needs them.

---

## 0. The shared harness (read this first — all four depend on it)

### 0.1 Command transport

Three macros push into the write-gather pipe at `0xCC008000`:

| macro | opcode | payload |
|---|---|---|
| `CGX_LOAD_BP_REG(x)` | `0x61` | one u32; the top 8 bits are the BP register index |
| `CGX_LOAD_CP_REG(a,v)` | `0x08` | u8 address, then u32 value |
| `CGX_BEGIN_LOAD_XF_REGS(addr,n)` | `0x10` | u32 = `((n-1) << 16) | addr`, then `n` u32/f32 words |

So every BP write in these tests is a single 32-bit word whose bits 24–31 are the register
number. An implementation must decode that fully; the tests never use libogc's cached GX
state for the registers under test (that is the whole point of `cgx` — see its header
comment about libogc's internal state being unsuitable for a test suite).

### 0.2 Viewport (`CGX_SetViewport`)

Writes 6 floats to XF `0x101A`:

```
wd = width/2,  ht = -height/2,  zrange = (far-near) * 16777215
ox = 342 + origin_x + width/2,  oy = 342 + origin_y + height/2,  oz = far * 16777215
```

The `342.0f` offset is the hardware's fixed screen-space origin bias (`342 = 0x156`).
The depth scale is `2^24 - 1`, not `2^24`. All three drawing tests set a 201x50 or
256x8 viewport this way and sample a single interior pixel, so the viewport transform only
has to be right enough to cover that pixel — but the 342 bias is load-bearing.

### 0.3 EFB copy (`CGX_DoEfbCopyTex`)

Writes, in order: `BPMEM_EFB_TL` (0x49) = top-left as X10Y10; `BPMEM_EFB_WH` (0x4A) =
(width-1, height-1); `BPMEM_MIPMAP_STRIDE` (0x4D) = `((width+3)>>2)*2` (hardcoded for an
RGBA8 destination); `BPMEM_EFB_ADDR` (0x4B) = physical destination `>> 5`; then
`BPMEM_TRIGGER_EFB_COPY` (0x52) = the `UPE_Copy` word.

`UPE_Copy` bit layout (BP 0x52), which `copyfilter` and `intensity` sweep directly:

| bits | field |
|---|---|
| 0 | `clamp_top` (default **true** in `EFBCopyParams`) |
| 1 | `clamp_bottom` (default **true**) |
| 2 | `unknown_bit` (a.k.a. "yuv"; `intensity` proves it does nothing) |
| 3–6 | `target_pixel_format`, stored *rotated*: `((fmt&7)<<1) | ((fmt&8)>>3)` |
| 7–8 | `gamma` (`GammaCorrection`: 0=1.0, 1=1.7, 2=2.2, 3=invalid-but-behaves-as-2.2) |
| 9 | `half_scale` (box filter 2:1) |
| 10 | `scale_invert` |
| 11 | `clear` |
| 12–13 | `frame_to_field` |
| 14 | `copy_to_xfb` |
| 15 | `intensity_fmt` |
| 16 | `auto_conv` |

All four tests copy as `EFBCopyFormat::RGBA8` (= 0x6, stored rotated as 0xC).

### 0.4 Tiled readback (`GXTest::ReadTestBuffer(s, t, width)`)

The destination is a standard GX RGBA8 texture: 4x4 tiles, each tile 64 bytes, split into
a 32-byte AR half followed by a 32-byte GB half. The address math is
`base = ((t>>2) * ((width+3)>>2) + (s>>2)) << 5`, `blkOff = ((t&3)<<2) + (s&3)`,
`offset = (base + blkOff) << 1`, then **A** is byte `offset+0`, **R** is `offset+1`,
**G** is `offset+32`, **B** is `offset+33`.

If our EFB-copy tiler gets this wrong, all four tests fail with channel-swapped garbage
rather than arithmetic mismatches — check this first when bringing them up.

### 0.5 `GXTest::GetTevOutput` — reading all 11 TEV bits through an 8-bit EFB

`tev.cpp` is the only consumer. The TEV output register is signed 11-bit
(`[-1024, 1023]`), but the EFB only stores 8 bits per channel, so the harness does two
render passes and reassembles:

**Pass 1 (low 6 bits).** Appends one extra TEV stage at `previous_stage + 1` whose **a**
input is the destination register and whose scale is `Scale4`. The a/b/c inputs truncate
to 8 bits rather than clamping to 11, which is why `a` is used and not `d` — the comment in
the harness notes this explicitly. The stage output is `4 * v`, the EFB keeps the low 8
bits, and the readback shifts right by 2. Result: `v & 0x3F`.

**Pass 2 (high 5 bits).** Appends three stages at `previous_stage + 1..3`, each with **d**
= the destination register and scale `Divide2` (each stage halves). Three stages give
`v >> 3` in the EFB; the readback shifts right by 3 again, giving `(v >> 6) & 0x1F`.

**Reassembly.** Treat the 5-bit pass-2 value as the top of an 11-bit two's-complement
number: if its bit 4 is set the contribution is `-1024 + ((hi & 0xF) << 6)`, otherwise
`hi << 6`; then add the 6 low bits. Net effect: sign-extend an 11-bit value into a host
`int` in `[-1024, 1023]`.

Constraints this imposes on us: the EFB format must be `RGB8_Z24` (the harness header says
so); at least 4 TEV stages must chain correctly through PREV; `Scale4` and `Divide2` must
be exact; and `GenMode.numtevstages` must be honoured mid-frame.

---

## 1. `tev.cpp` — the TEV combiner and the konst table

### 1.1 What it asserts

Two test blocks:

**`TevCombinerTest()`** — three phases.

| phase | subtests | what |
|---|---|---|
| 11-bit round-trip | **2047** | For every value of TEV register c0.r from -1024 to 1022, set a one-stage TEV whose **d** input is `c0.rgb` and confirm `GetTevOutput` returns exactly that value. This validates the harness itself and that c0 really stores signed 11 bits. |
| randomised combiner | **61440** (`0xF000`) | Random `scale` (4), `bias` (3 of 4 — Compare is excluded), `op` (2), `clamp` (2), with `a = c0.rgb`, `b = c1.rgb`, `c = c2.rgb`, `d = ZERO`, and random signed 11-bit values in `a`, `b`, `c` (`d` is pinned to 0). Compares against a C++ reimplementation of the hardware formula. |
| compare-mode probe | **2** | Sets the *colour* combiner's a/b to `c2.rgb`/`c1.rgb` and the *alpha* combiner's a/b to `c0`/`c1`, with alpha bias = `Compare`. Asserts the alpha combiner reads a and b from the **colour** combiner's selection, not its own. Expected results 0 then 255. |

Total ≈ **63,489** assertions. Note phase 1's `i == 1` branch (the RGBA6 6-bit truncation
probe) `break`s immediately — it is dead and marked TODO upstream. Its intended expectation
is recorded in §1.3 anyway because we will eventually want it.

Also note the in-source finding: with a single stage, `TevColorArg::PrevColor` as the
**d** input does not appear to read PREV. The test therefore uses `ZERO` for `d`, which is
why `d` is always 0 in the randomised sweep — the `d` term of the formula is **untested**
by this suite.

**`KonstTest()`** — **65** subtests: 64 steps sweeping every `kcsel0` value (steps 0–31)
and every `kasel0` value (steps 32–63), plus one check that `TevColorArg::Half` yields
exactly 128.

### 1.2 GPU features needed

- **No texturing whatsoever.** `TwoTevStageOrders` (BP 0x28) has `texmap0 = GX_TEXMAP_NULL`,
  `texcoord0 = GX_TEXCOORDNULL`, `enable0 = false`, `colorchan0 = Color0`;
  `GenMode.numtexgens = 0`. Only the rasterised colour path and the TEV register file
  matter.
- **XF**: `0x1009` numchans = 1; `0x100E`/`0x1010` = colour/alpha channel 0 control
  (`LitChannel`, lighting **off**; `matsource = 1` = from-vertex in `TevCombinerTest`,
  `matsource = 0` = from-register in `KonstTest`); `0x1005` clip-disable = 0;
  `0x101A` viewport; orthographic projection matrix.
- **BP**: `GENMODE` 0x00 (`numtevstages`, `numtexgens`, `numcolchans`, `cullmode = None`);
  `TREF` 0x28; `TEV_COLOR_ENV` 0xC0 + 2*stage and `TEV_ALPHA_ENV` 0xC1 + 2*stage;
  `TEV_COLOR_RA` 0xE0 + 2*reg and `TEV_COLOR_BG` 0xE1 + 2*reg for **both**
  `TevRegType::Color` (prev/c0/c1/c2) and `TevRegType::Constant` (konst k0..k3);
  `TEV_KSEL` 0xF6 (`kcsel0` bits 4–8, `kasel0` bits 9–13, `kcsel1` 14–18, `kasel1` 19–23);
  `ZMODE` 0x40; `ZCOMPARE`/`PEControl` 0x43; `BLENDMODE` 0x41 (blending explicitly off,
  logic op off, dither on, colour+alpha update on); the EFB-copy block 0x49/0x4A/0x4B/0x4D/0x52.
- **TEV input selectors exercised**: `Color0/1/2`, `PrevColor`, `PrevAlpha`, `Alpha0/1/2`,
  `Konst` (colour and alpha), `Half`, `Zero`. `RasColor` is *not* read by `KonstTest`'s
  combiners (the quad's vertex colour is black with alpha 255 and is discarded).
- **EFB pixel formats**: `RGB8_Z24` for everything except the compare-mode probe, which uses
  `RGBA6_Z24`.
- **Signed 11-bit TEV registers** with correct 8-bit truncation of the a/b/c inputs and
  11-bit behaviour on `d`.
- Multi-stage chaining (up to stage index `previous_stage + 3`) with PREV forwarding
  between stages — `KonstTest` uses 2 stages, `GetTevOutput` adds up to 3 more.

### 1.3 The exact arithmetic

**The combiner reference model** (`TevCombinerExpectation`, the most valuable formula in
the whole suite). Given signed inputs `a`, `b`, `c`, `d`, a `scale`, a `bias`, an `op` and
a `clamp` flag:

1. **Truncate the multiplier inputs to 8 bits**: `a, b, c ← a & 255, b & 255, c & 255`.
   `d` is *not* truncated — it stays the full 10/11-bit signed value. (Upstream comment:
   "abc=8bit, d=10bit".)
2. **Widen the interpolation weight from 8 to 9 bits**: `c' = c + (c >> 7)`. So `c = 255`
   maps to `256` and the weight spans `[0, 256]` inclusive, not `[0, 255]`.
3. **Derive the shifts**: `lshift = 1` for `Scale2`, `2` for `Scale4`, `0` for `Scale1`
   *and* `Divide2`. `rshift = 1` for `Divide2`, else `0`.
4. **Derive the rounding bias**: `0` if `scale == Divide2`; otherwise `127` if `op == Sub`
   and `128` if `op == Add`. **This asymmetry between add and subtract is real hardware
   behaviour and is the single easiest thing to get wrong.**
5. **Scaled, rounded lerp**:
   `L = ((a*(256 - c') + b*c') << lshift) + round_bias) >> 8`
   Note the ordering: the product is scaled *before* the rounding bias is added and
   *before* the `>> 8`. Doing `lerp` then `scale` gives different low bits.
6. **Combine with d**: `E = (d << lshift) + (op == Sub ? -L : +L)`.
   `d` is shifted by the same `lshift`.
7. **Apply the bias term**, also shifted: `E += (bias == AddHalf ? +128 : bias == SubHalf ? -128 : 0) << lshift`.
8. **Divide-by-2 last**: `E >>= rshift` (arithmetic shift of the already-summed signed value,
   applied after everything else).
9. **Clamp**: if `clamp` then to `[0, 255]`, else to `[-1024, 1023]`.

`bias == Compare` is explicitly not modelled by this formula (`TevBias::Compare` re-purposes
the `op` bit as `comparison` and the `scale` field as `compare_mode` — see `BPMemory.h`).
The compare-mode probe only checks the *input routing*, not the comparison arithmetic.

**The 6-bit EFB truncation model** (the dead `i == 1` branch, for when we get there): the
expected 8-bit readback of an 11-bit value `v` written to an `RGBA6_Z24` EFB is
`t = (((v + 1) >> 2) & 0xFF) << 2`, then `t | (t >> 6)`. That is: round-to-nearest on the
two dropped LSBs, truncate to 8 bits, and on readback replicate the top 2 bits of the
6-bit field into the bottom 2. The upstream note says this does not yet match hardware, so
treat the formula as a hypothesis, not a spec.

**The konst table.** `KonstTest`'s hardcoded expectations pin the hardware's constant
ladder. For `kcsel` (colour) selectors 0–7 and `kasel` (alpha) selectors 0–7, the
generated 8-bit value is:

| sel | 0 (1) | 1 (7/8) | 2 (3/4) | 3 (5/8) | 4 (1/2) | 5 (3/8) | 6 (1/4) | 7 (1/8) |
|---|---|---|---|---|---|---|---|---|
| value | 255 | 223 | 191 | 159 | **128** | 96 | 64 | 32 |

This is *not* `round(255 * n/8)` and *not* `32*n` — note 4/8 gives **128**, not 127 or 128
consistently with its neighbours, while 7/8 gives 223 rather than 224. Store it as a
lookup table.

Selectors 8–11 yield 0 for both colour and alpha. Selectors 12–15 (`K0`–`K3`) are
valid only for **colour** (they select the konst register's RGB); for **alpha** they are
invalid and yield 0 — the expectation array has zeros at indices 40–47 for exactly this
reason. Selectors 16–31 are the per-component selectors `K{0..3}_{R,G,B,A}`, valid for
both. The test seeds konst register *i* with `r = 10 + 50i`, `g = 20 + 50i`,
`b = 30 + 50i`, `a = 40 + 50i` and reads back exactly those, confirming the ordering.

`TevColorArg::Half` produces exactly **128**.

---

## 2. `lighting.cpp` — the material x channel-colour multiply

### 2.1 What it asserts

**65,536** subtests: the full 256x256 cross product of an 8-bit material colour and an
8-bit ambient colour. For each pair it draws a quad, EFB-copies, reads one interior pixel,
and checks the red channel against a one-line formula.

The name is slightly misleading. **No lights are enabled.** `LitChannel.hex` is zeroed
before setting `matsource = 0` (register), `ambsource = 0` (register),
`enablelighting = true` — which leaves `lightMask0_3` and `lightMask4_7` at zero, so
`GetFullLightMask()` returns 0. With no lights contributing, the accumulated illumination
is just the ambient register, and the test reduces to a pure study of **how the hardware
rounds the final material x illumination product**, which is the stated intent in the
source comment.

### 2.2 GPU features needed

This is by far the smallest feature set of the four.

- **XF registers**: `0x1009` numchans = 1; `0x100A` = ambient colour register 0, written as
  `(amb << 24) | 255` i.e. R = amb, G = 0, B = 0, A = 255; `0x100C` = material colour
  register 0, written as `(mat << 24) | 255`; `0x100E` / `0x1010` = colour / alpha channel
  0 control; `0x1005` clip-disable = 0; `0x101A` viewport (201 x 50).
- **Not needed**: the XF light registers (`0x0600`–`0x067F`), `diffusefunc`, `attnfunc`,
  vertex normals, the normal matrix. `LitChannel.diffusefunc` and `.attnfunc` are both 0
  (`None` / `Spec`-encoded) but are never reached because the light mask is empty. The
  vertex format is position + RGBA8 colour only. **A correct answer here needs the channel
  colour *plumbing*, not the per-light loop.**
- **BP**: `TREF` 0x28 with texturing off and `colorchan0 = Color0`; one TEV stage whose
  **d** input is `RasColor`; `ZMODE` 0x40 (test off, update off); `PEControl` 0x43 with
  `RGB8_Z24` / `ZLINEAR` / early-Z off; the EFB copy block. `TevReg` c0 is set to red 0xFF
  but is unused by the combiner.
- **Real texturing: no.** Only the rasterised colour needs to reach TEV.

### 2.3 The exact arithmetic

For each channel:

```
expected = (material * (illum + (illum >> 7))) >> 8
```

where `material` is the 8-bit material-register component and `illum` is the 8-bit
accumulated channel colour (here, the ambient register component).

Two things to internalise:

1. **The same 8→9-bit widening as the TEV `c` input**: `illum + (illum >> 7)` maps `[0,255]`
   onto `[0,256]`, so full-white ambient is a true unity multiplier
   (`255 * 256 >> 8 = 255`).
2. **Pure truncation, no rounding bias.** Unlike the TEV lerp in §1.3, there is *no* `+128`
   before the `>> 8`. The 16-bit product is simply shifted down. Implementing this with a
   rounded multiply produces off-by-one errors across roughly half the 65,536 pairs.

Because the ambient/material writes put the swept byte in bits 24–31, only the **red**
channel is meaningfully swept; G and B are 0 and A is 255 for both registers.

---

## 3. `intensity.cpp` — the EFB-copy intensity (YUV) format

### 3.1 What it asserts

Per invocation of `IntensityTest`: **262,144** subtests = 256 x 256 pixels x 4 channel
assertions (R/Y, G/U, B/V, A). `main()` calls it `256 (blue) x 8 (flag combos) = 2048`
times, so the full run is ~537M assertions — the HOME-button early-out exists for exactly
this reason. In practice pick one or a few `blue` values.

The sweep is over:
- `blue` 0..255 — a constant blue plane; R = x, G = y, so the 256x256 EFB region covers
  every (R, G) pair at a fixed B.
- the 3-bit `counter`, which drives `unknown_bit` (bit 0), `intensity_fmt` (bit 1),
  `auto_conv` (bit 2).

The assertion: conversion happens **iff `intensity_fmt && auto_conv`**. Otherwise the copy
is a straight RGB pass-through with alpha forced to 255. `unknown_bit` (BP 0x52 bit 2) is
swept purely to prove it has **no** observable effect — the source note records that
Dolphin renamed it to "yuv" and that the rename seems unjustified.

### 3.2 GPU features needed

The smallest *dependency* set of the four: **no rasteriser, no TEV, no vertex pipeline at
all.**

- **PE pokes**: `GX_PokeARGB` over 256x256, with poke state set to dither **off**, alpha
  update on, colour update on, blend mode `GX_BM_NONE` with `Zero`/`Zero` factors and
  `LogicOp::Set`, alpha read `GX_READ_NONE`, and poke Z-mode (compare off, `Always`,
  update on).
- **BP**: `PEControl` 0x43 with `RGB8_Z24` / `ZLINEAR` / early-Z off; the EFB-copy block
  0x49/0x4A/0x4B/0x4D/0x52 with `intensity_fmt`, `auto_conv`, `unknown_bit`.
- **EFB copy** of a 256x256 region to an RGBA8 tiled texture, and the tiled readback of §0.4.
- **Gotcha**: this test never writes the copy-filter registers (BP 0x53/0x54). It inherits
  whatever `GXTest::Init` → libogc's `GX_SetCopyFilter(rmode->aa, ..., 1, rmode->vfilter)`
  left behind, and relies on that being unity gain. Our copy path's default filter state
  must be a pass-through or this test fails with values scaled by the wrong weight sum.

### 3.3 The exact arithmetic

BT.601 studio-swing RGB→YCbCr in 8-bit fixed point. Given the filtered 8-bit `r`, `g`, `b`:

```
y_raw =  +66*r + 129*g +  +25*b     (unsigned 16-bit; max 220*255 = 56100, fits)
u_raw =  -38*r + -74*g + 112*b      (signed 16-bit; range +/- 28560)
v_raw = +112*r + -94*g + -18*b      (signed 16-bit)
```

Then, per component, a round-half-up on the 8 discarded fractional bits plus a level offset:

```
Y = u8( (y_raw >> 8) + ((y_raw >> 7) & 1) + 16  )
U = u8( (u_raw >> 8) + ((u_raw >> 7) & 1) + 128 )
V = u8( (v_raw >> 8) + ((v_raw >> 7) & 1) + 128 )
A = alpha, unchanged
```

Details that matter for bit-exactness:

- The `((x >> 7) & 1)` term is the rounding: it adds back the MSB of the fraction being
  discarded. Applied **after** the arithmetic shift, and on the *signed* value for U and V.
- `u_raw` and `v_raw` are **signed 16-bit**, so their `>>` is an arithmetic shift.
  `y_raw` is unsigned.
- The final narrowing to `u8` is a **wrap, not a saturate**. Out-of-range sums roll over.
  This is deliberate and is a large part of what the 262,144-point sweep is checking.
- The coefficient triples sum to 220 (Y), 0 (U), 0 (V); offsets are +16 / +128 / +128.
- Result channel mapping: Y lands in R, U in G, V in B.

The pass-through case (when not both flags are set) is `{x, y, blue, 255}` — note alpha
becomes **255** even though the EFB was poked with alpha 255 anyway; in `RGB8_Z24` there is
no alpha stored.

---

## 4. `copyfilter.cpp` — the vertical copy filter, gamma, and EFB formats

### 4.1 What it asserts

The most expensive test in the set. Structure:

```
for each pixel_fmt in {RGB8_Z24, RGBA6_Z24, Z24}:        # FULL_PIXEL_FORMATS off
    FillEFB(pixel_fmt); CheckEFB(pixel_fmt)
    for copy_filter_sum in 0..189:                       # 63*3, FULL_COPY_FILTER_COEFS on
        for gamma in {1.0, 1.7, 2.2, invalid-2.2}:       # FULL_GAMMA on
            for flags in 2..15:                          # CHECK_PREV_AND_NEXT on
                CopyFilterTest(...)
```

- `CheckEFB` is **8,192** assertions for the colour formats (256 x x 8 y x 4 channels) and
  **2,048** for `Z24` (depth only). It validates the EFB's own bit-depth behaviour through
  PE peeks, independently of any copy.
- `CopyFilterTest` runs **190 x 4 x 14 = 10,640** times per pixel format (**31,920** total
  with the three default formats), each block asserting **1,024** times (256 columns x 4
  channels) at row y = 4.

So the "~12,000 subtests" figure corresponds to roughly one pixel format's configuration
grid (`190 sums x 4 gammas x 16 flag combos = 12,160` is the upper bound; the loop skips
flags 0 and 1, giving 10,640 actual). The sweep dimensions are: **the per-row filter
weight sum (0..189), the gamma setting (4), which of prev/current/next rows receive that
weight (3 independent bits), whether the intensity format is applied (1 bit), and the EFB
pixel format (3).**

The `flags` loop starts at 2 rather than 0 to skip the degenerate all-rows-zero case,
which is already covered by `copy_filter_sum = 0`. When the requested sum exceeds a row
group's capacity it saturates: current row caps at `63*3 = 189`, previous and next rows at
`63*2 = 126`.

### 4.2 GPU features needed

Everything `intensity.cpp` needs, **plus**:

- **Copy filter registers**: `BPMEM_COPYFILTER0` (0x53) holding `w0..w3` and
  `BPMEM_COPYFILTER1` (0x54) holding `w4..w6`, as a 64-bit `CopyFilterCoefficients` union
  with seven **6-bit** fields at bit offsets 0/6/12/18 (low word) and 32/38/44 (high word).
  Row grouping: `w0, w1` → previous row; `w2, w3, w4` → **current** row; `w5, w6` → next
  row. The test spreads a desired row-sum across the available fields, filling `w3` first,
  then `w2`, then `w4`.
- **Gamma** in the copy trigger (BP 0x52 bits 7–8), all four encodings.
- **Three EFB pixel formats** — `RGB8_Z24`, `RGBA6_Z24` and `Z24` — with correct bit-depth
  reduction, both on copy and on peek.
- **PE peeks** for colour (`GX_PeekARGB`) and depth (`GX_PeekZ`).
- **For the `Z24` case only**, a substantial extra feature set, because `GX_PokeZ` does not
  work: the test EFB-copies the RGB8 contents into `test_buffer`, binds that buffer as a
  256x8 `RGBA8` texture and redraws it with z-texture replacing depth. That needs:
  - `TX_SETMODE0`/`TX_SETMODE1`, `TexImage0` (width-1, height-1, format `RGBA8`),
    `TexImage1`/`TexImage2` (TMEM; left to libogc's init), `TexImage3`
    (`image_base` = physical address `>> 5`) — i.e. **real texture sampling**;
  - one texgen: XF `0x1008` VTXSPECS = `1 << 4`, XF `0x1009` numchans = 0, XF `0x103F`
    numtexgens = 1, XF `0x1040` texmtxinfo = `0x280` (regular texgen for tex0);
  - `SU_SSIZE` (0x30) / `SU_TSIZE` (0x31) with `scale_minus_1` = 255 / 7;
  - **z-texture**: `BPMEM_BIAS` (0xF4) = 0 and `ZTex2` (0xF5) with `type = U24`,
    `op = Replace`;
  - `AlphaTest` (0xF3) with both comparisons `Always` / `Or`;
  - `TwoTevStageOrders` with `texmap0 = 0`, `texcoord0 = 0`, `enable0 = true`;
  - direct CP vertex setup: CP `0x50`/`0x60` (VCD lo/hi: direct position, direct texcoord0),
    CP `0x70` VAT group 0 with `PosElements = XYZ`, `PosFormat = S8`,
    `Tex0CoordElements = ST`, `Tex0CoordFormat = U8`; CP `0x80` = `0x80000000`
    (vcache enhance); CP `0x90` = 0. A source note records that using XY positions
    (omitting Z) breaks it even though Z is irrelevant to the result.
  - `BPMEM_CLEAR_Z` (0x51) set to a recognisable sentinel (`123456`) that should be
    overridden.

- **Row clamping is not exercised at the assertion point.** `clamp_top` and `clamp_bottom`
  default to true and the copy region is rows 0..7, so rows 3/4/5 are interior.

### 4.3 The exact arithmetic

**(a) The EFB source pattern.** For column `x`, row `y`:
`r = x & 255`, `b = x & 255`, `a = x & 255`, and
`g = (y == 4 ? x : (y == 3 ? 255 : 0))`.
So the row under test (4) has a ramp in green, the previous row (3) is green 255, and the
next row (5) is green 0 — which makes the three filter taps individually identifiable in
the green channel while R and B stay constant down the column. Depth, where used, is
`(r << 16) | (g << 8) | b` of that same generated colour.

**(b) EFB bit-depth reduction** (`PredictEfbColor`), the quantisation the EFB applies
before the filter sees the data:

- `RGB8_Z24` (and `YUV420`): pass through, alpha forced to **255**.
- `RGBA6_Z24`: every channel *including alpha* goes through a 6-bit round trip
  `SixBit(v) = (v & 0xFC) | ((v & 0xC0) >> 6)` — keep the top 6 bits, then replicate the
  top **2** bits into the bottom 2.
- `RGB565_Z16`: `FiveBit(v) = (v & 0xF8) | ((v & 0xE0) >> 5)` for R and B (top 5 bits, top
  3 replicated into the bottom 3), `SixBit` for G, alpha 255. **Not actually reachable** —
  `FillEFB` and `CheckEFB` both bail out on this format because `GX_PokeARGB` hangs.
- `Z24`: the copy reads the 24-bit depth as `r = depth >> 16`, `g = (depth >> 8) & 255`,
  `b = depth & 255`, `a = 255`.
- `Y8` / `U8` / `V8`: only under `FULL_PIXEL_FORMATS` (off). These are documented upstream
  as not making sense and as diverging between peeks and copies:
  `Y8Transform(v) = (v <= 1) ? 0 : 255`;
  `U8Transform(v) = 0` if `v <= 1`, `255` if `v` is odd, else `12 + 65*((v & 0xC0) >> 6) + 32*((v & 2) >> 1)`;
  `V8Transform(v) = v` if odd, else `FiveBit(v)`. For `Y8`, copies pass through unchanged
  while peeks apply the transform; for `U8`, only peeks work at all and copies are recorded
  as unknown (predicted as black). **Do not treat these as a spec — they are observations.**

**(c) The copy filter + gamma, per colour channel** (`Predict`). Given the three
bit-reduced source values `prev`, `cur`, `next` and the three programmed row weight sums
`W_prev`, `W_cur`, `W_next`:

> **How the seven register weights make three row sums.** The filter has seven six-bit
> weights, four in BP `0x53` (w0..w3, at bits 0, 6, 12, 18) and three in BP `0x54`
> (w4..w6). `copyfilter.cpp`'s own `SetCopyFilter` shows the grouping, and it is *not*
> three-one-three:
>
> - previous row = **w0 + w1**
> - current row = **w2 + w3 + w4**
> - next row = **w5 + w6**
>
> This matters immediately. libogc's `GX_Init` leaves `{0, 0, 21, 22, 21, 0, 0}`, which
> under this grouping is prev 0, current 64, next 0 - an exact pass-through, which is
> what `intensity.cpp` needs and assumes. Under a three-one-three grouping the same
> registers would be a 21/22/21 vertical blur, and `intensity` would fail on its last
> row. Checked against the test's own code, 2026-09-12.

1. **Weighted sum in 32-bit unsigned**:
   `acc = prev*W_prev + cur*W_cur + next*W_next`.
2. **Divide by 64**: `acc >>= 6`. So a total weight of 64 is unity gain; the maximum
   programmable total is `189 + 126 + 126 = 441`, i.e. up to ~6.9x gain.
3. **Wrap to 9 bits before clamping**: `acc &= 0x1FF`. **This is the subtle part.** The
   hardware's clamp operates on a 9-bit quantity, so a value above 511 wraps around and is
   then clamped, producing a *low* result instead of white. The source comment pins the
   threshold: this happens once `copy_filter_sum >= 128`. An implementation that clamps
   directly to 255 will pass the low half of the sweep and fail the top half.
4. **Clamp**: `acc = min(acc, 255)`.
5. **Gamma**, only when the setting is not 1.0:
   - normalise `f = acc / 255.0f` (single precision),
   - `f = powf(f, 1/1.7f)` for `Gamma1_7`; `f = powf(f, 1/2.2f)` for **both** `Gamma2_2`
     and the "invalid" encoding 3 — the invalid encoding is confirmed by hardware to
     behave identically to 2.2,
   - `acc = round(f * 255.0f)` (round-half-away-from-zero).
   No overflow is possible here: `pow` maps `[0,1]` to `[0,1]`.
6. **Alpha bypasses the whole thing.** Alpha is taken directly from the *current* row's
   bit-reduced EFB alpha — it is neither filtered nor gamma-corrected.
7. **Then, if `intensity_fmt`**, the filtered and gamma-corrected RGB is run through the
   BT.601 conversion of §3.3, with identical coefficients, identical `>> 8` +
   `((x >> 7) & 1)` rounding, identical `+16`/`+128`/`+128` offsets, and alpha again
   passing through untouched. Note the test sets `auto_conv = intensity_fmt` here, so the
   two flags always agree and the §3.2 "both required" condition is satisfied.

**Ordering is load-bearing**: filter → 9-bit wrap → clamp → gamma → intensity. Swapping
gamma and intensity, or clamping before the wrap, changes results.

---

## 5. Recommended implementation order

Ranked cheapest-first by the size of the feature set each one requires.

### 1st — `intensity.cpp` *(testable soonest)*

It is the only one of the four with **no dependency on the rasteriser, the vertex pipeline,
the TEV, or lighting**. It needs exactly three things: PE `PokeARGB`, an EFB→RGBA8 tiled
copy, and a ~10-line BT.601 conversion function. If the EFB-copy tiler and PE poke path
already work, this becomes green the same day. It also has the useful property that it is a
*pure* test of the copy path — a failure here is unambiguous, whereas a failure in the other
three could be anywhere upstream.

Scope it to a handful of `blue` values (each is 262,144 assertions); all eight flag combos
are cheap once one blue value passes.

Blockers to clear first: the tiled readback layout (§0.4) and a unity-gain default copy
filter (§3.2 gotcha).

### 2nd — `lighting.cpp`

Adds the quad rasteriser, the XF colour-channel path and one TEV stage reading `RasColor` —
but *not* lights, normals, attenuation, diffuse functions or texturing. The arithmetic under
test is a single multiply-widen-shift. Good second step because it proves the draw →
rasterise → TEV → EFB → copy chain end-to-end with the simplest possible content, and the
formula it validates (`(mat * (illum + (illum >> 7))) >> 8`) is the same 8→9-bit widening
that reappears as the TEV `c` input, so getting it right pays forward.

### 3rd — `tev.cpp`

Needs the full TEV combiner with exact fixed-point semantics, signed 11-bit registers,
multi-stage PREV chaining, the konst selection table, and both `RGB8_Z24` and `RGBA6_Z24`
EFB formats. Two mitigations make it less painful than it sounds:
- the combiner formula of §1.3 can be ported into a **host-side unit test** and validated
  against an exhaustive or randomised sweep *before* any GPU work, decoupling the arithmetic
  from the pipeline;
- `GetTevOutput`'s two-pass readback exercises the pipeline hard, so bring up
  `lighting.cpp` first to prove the draw/copy chain is sound, or you will be debugging two
  things at once.

Note the suite leaves the `d` term of the combiner effectively untested (`d` is pinned to 0
in the randomised sweep) and does not test compare-mode arithmetic at all — we should not
mistake a green `tev.cpp` for full combiner coverage.

### 4th — `copyfilter.cpp`

A strict superset of `intensity.cpp` plus: the 7-tap vertical filter with its 9-bit
wrap-before-clamp, float gamma (three curves), three EFB pixel formats with their bit-depth
round trips, PE peeks for colour and depth, and — for the `Z24` case — real texture
sampling, z-textures, an extra texgen, and hand-rolled CP vertex format setup. Do it last,
and consider staging it: get `RGB8_Z24` + gamma 1.0 passing first (that is the filter
arithmetic alone), then gamma, then `RGBA6_Z24`, and treat the `Z24`/z-texture path as a
separate milestone since it pulls in the entire texture unit.

---

## 6. Cross-cutting notes

- **Three separate 8→9-bit widenings** appear across the suite and use the same trick
  (`v + (v >> 7)`): the TEV `c` interpolation weight (§1.3 step 2), the lighting illumination
  factor (§2.3), and — in a different guise — the 6-bit-to-8-bit EFB expansion
  (`SixBit`, which replicates the top 2 bits). Implement one helper and reuse it.
- **Rounding is not uniform.** TEV lerp rounds with a bias that depends on the op
  (128 add / 127 sub / 0 for divide-2); lighting does not round at all; the copy filter
  truncates; BT.601 rounds half-up from the discarded MSB; gamma rounds half-away-from-zero
  in float. Each of these is a distinct rule and none generalises to the others.
- **Saturation is not uniform either.** TEV clamps to `[0,255]` or `[-1024,1023]` depending
  on a flag; the copy filter wraps to 9 bits *then* clamps to 255; BT.601 wraps to 8 bits
  with no clamp at all.
- **Everything funnels through the EFB copy and the 4x4 tiled RGBA8 readback.** That path is
  the single highest-leverage thing to get right — it gates all four tests.
