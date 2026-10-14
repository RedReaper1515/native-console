# GameCube Hardware Lighting: Colour Channels, Lights and a Test Plan

This document is written for one job: **writing a hardware test program for the transform unit's
(XF) per-vertex lighting, and checking an emulator against it.** It restates the register layout
and the equation in one place, says precisely which parts are established and by what, and ends
with a set of test cases (Part F) chosen so that each likely implementation mistake produces a
number that differs from the right one by far more than a rounding step.

The single most important framing fact: **the only part of the lighting datapath that has been
measured on silicon is the last multiply** — material times the clamped illumination, with no light
in the scene. The existing `gxtest_lighting` case (65,536 material/ambient pairs) verifies that and
nothing else. Every statement below about what a *light* does comes from a header comment, from
libogc's setup code, or from a second implementation, and is labelled accordingly.

Companion documents in this folder:

* [`gc-xf-registers.md`](gc-xf-registers.md) — the whole XF address space and FIFO encoding.
* [`gc-lighting-texgen.md`](gc-lighting-texgen.md) — the lighting arithmetic plus texgen, including
  the derivations of `GX_InitLightSpot()` / `GX_InitLightDistAttn()` coefficients (its §A.5), which
  are not repeated here.

Where this document and those disagree, §G lists the disagreement.

## Provenance

Six sources were read directly. **No GPL code, comment text or closely paraphrased wording has been
copied into this document.** GPL material is described in prose as observed behaviour, and every
such claim is labelled "Dolphin behaviour" — a second implementation, not hardware.

1. **devkitPro/libogc `libogc/gx.c`** (zlib licence), fetched 2026-09-12 from
   `https://raw.githubusercontent.com/devkitPro/libogc/master/libogc/gx.c` into the session
   scratchpad (5199 lines; line numbers refer to that revision, which matches the copy cited by the
   companion documents). **Primary authority** for what the GX API writes into which XF address.
   Code quoted in this document comes only from here and from items 2–3.
2. **devkitPro/libogc `gc/ogc/gx.h`** (zlib), same fetch, 5328 lines; byte-identical to
   `D:\devkitPro\libogc\include\ogc\gx.h`. Enum values, and the API documentation comments that
   state the attenuation equation, the coordinate-space requirements and the specular model.
3. **devkitPro/libogc `libogc/gx_regdef.h`** (zlib), same fetch — the layout of the light object
   (`struct __gx_litobj`, `gx_regdef.h:85-101`).
4. **`runtimes/runtime-gc/src/gc_gx_draw.c`** in this repository — the implementation under test. Read to
   predict what it will output for each Part F case; nothing in it is treated as evidence of
   hardware behaviour.
5. **dolphin-emu/hwtests `gxtest/lighting.cpp`** (GPL-2.0), identical to the vendored copy at
   `roms/gc/hwtests/gxtest/lighting.cpp`. **Behaviour reference only.** It supplies the one
   silicon-derived fact here (§C.2). Note that it is built against `wiiuse/wpad.h`
   (`lighting.cpp:10`), i.e. it is a **Wii** program; that Flipper rounds identically to Hollywood is
   assumed, not shown.
6. **Dolphin** (GPL-2.0-or-later), master branch fetched 2026-09-12: `VideoCommon/XFMemory.h`,
   `VideoCommon/LightingShaderGen.cpp`, `VideoCommon/VertexShaderGen.cpp`,
   `VideoCommon/VertexShaderManager.cpp`, `VideoBackends/Software/TransformUnit.cpp`.
   **Behaviour reference only.** Dolphin contains *two* lighting implementations — the GPU shader
   generator ("Dolphin-HW" below) and the software renderer ("Dolphin-SW") — and **they disagree
   with each other** on rounding, on light-direction normalisation and on zero denominators. Those
   disagreements are the best available map of what nobody has pinned down, and they drive several
   Part F cases.

Anything not established by items 1–3 or 5 is marked **Unverified:**, with what would settle it.

Helper macros, as in libogc: `_SHIFTL(v,s,w)` places the low `w` bits of `v` at bit `s`;
`_SHIFTR(v,s,w)` extracts `w` bits from bit `s`.

---

# Part A — Registers and memory

## A.1 Address map

| XF address | Size | Contents | Written by (libogc) |
|---|---|---|---|
| `0x0000 + 4·i` | 12 words | Position matrix row block *i* (3 rows × 4) | `GX_LoadPosMtxImm()` — `gx.c:2872-2876` |
| `0x0400 + 3·i` | 9 words | Normal matrix for position index *i* (3 rows × 3) | `GX_LoadNrmMtxImm()` — `gx.c:2884-2888` |
| `0x0600 + 16·n` | 16 words | Light *n*, *n* = 0..7 | `GX_LoadLightObj()` — `gx.c:4587-4641` |
| `0x1008` | 1 | Vertex specs (colour count, normal count, texcoord count) | `__GX_XfVtxSpecs()` — `gx.c:751-776` |
| `0x1009` | 1 | Number of colour channels output | `__GX_SetChanCntrl()` — `gx.c:1010` |
| `0x100A` / `0x100B` | 1 each | Ambient colour, channel 0 / 1 | `__GX_SetChanColor()` — `gx.c:996-999` |
| `0x100C` / `0x100D` | 1 each | Material colour, channel 0 / 1 | `__GX_SetChanColor()` — `gx.c:1000-1003` |
| `0x100E` / `0x100F` | 1 each | Colour channel control, channel 0 / 1 | `__GX_SetChanCntrl()` — `gx.c:1012-1020` |
| `0x1010` / `0x1011` | 1 each | Alpha channel control, channel 0 / 1 | same |
| `0x1018` | 1 | Matrix index A; bits 5:0 = current position/normal matrix index | `__GX_SetMatrixIndex()` — `gx.c:778-788` |

All of these go through the FIFO command `0x10`: one byte `0x10`, one 32-bit word
`((count−1) << 16) | address`, then `count` data words (`GX_LOAD_XF_REG` / `GX_LOAD_XF_REGS`,
`gx.c:46-62`). A test program can write any address directly with that sequence, which is how the
register-only cases in Part F are reached.

## A.2 Channel control word (`0x100E`–`0x1011`)

The complete construction, `GX_SetChanCtrl()`, `gx.c:2195-2196`:

```c
u32 reg,difffn = (attn_fn==GX_AF_SPEC)?GX_DF_NONE:diff_fn;
u32 val = (matsrc&1)|(_SHIFTL(enable,1,1))|(_SHIFTL(litmask,2,4))|(_SHIFTL(ambsrc,6,1))|(_SHIFTL(difffn,7,2))|(_SHIFTL(((GX_AF_NONE-attn_fn)>0),9,1))|(_SHIFTL((attn_fn>0),10,1))|(_SHIFTL((_SHIFTR(litmask,4,4)),11,4));
```

| Bit(s) | Field | Values | Source |
|---|---|---|---|
| 0 | Material source | 0 = register (`GX_SRC_REG`), 1 = vertex colour (`GX_SRC_VTX`) | `gx.h:172-173` |
| 1 | Lighting enable | 0 = output is the material, untouched; 1 = run the lighting equation | `gx.h:1846-1848` |
| 5:2 | Light mask, lights 0–3 (bit 2 = light 0) | | `gx.c:2196`, `gx.h:178-181` |
| 6 | Ambient source | 0 = register, 1 = vertex colour | `gx.h:172-173` |
| 8:7 | Diffuse function | 0 = none, 1 = signed, 2 = clamp, 3 = never written | `gx.h:193-195` |
| 10:9 | Attenuation function (register encoding, **not** the API enum) | see below | `gx.c:2196` |
| 14:11 | Light mask, lights 4–7 (bit 11 = light 4) | | `gx.c:2196`, `gx.h:182-185` |
| 31:15 | Unused by libogc | | **Unverified:** whether any are live |

So the register bit for light *k* is **bit 2+k for k < 4 and bit 7+k for k ≥ 4**.

The attenuation field is derived arithmetically from the API enum (`GX_AF_SPEC` = 0, `GX_AF_SPOT` =
1, `GX_AF_NONE` = 2 — `gx.h:201-203`). Evaluating the two terms:

| API constant | bit 9 = `(2 − attn_fn) > 0` | bit 10 = `attn_fn > 0` | Register value 10:9 |
|---|---|---|---|
| `GX_AF_SPEC` (0) | 1 | 0 | **1** |
| `GX_AF_SPOT` (1) | 1 | 1 | **3** |
| `GX_AF_NONE` (2) | 0 | 1 | **2** |
| — | — | — | 0: never written by libogc |

**Register values: 1 = specular, 2 = none, 3 = spot.** Dolphin behaviour agrees on 1/2/3 and treats
0 exactly like 2 (`XFMemory.h:159-165`, `LightingShaderGen.cpp:25-30`). **Unverified:** what
hardware does with 0.

**Specular forces the written diffuse field to 0** (`gx.c:2195`). A register value with attenuation
1 and a non-zero diffuse field is reachable only by a direct write; §B.4.3 (item 3) shows why that is not a
no-op in Dolphin.

Registers `0x100E` and `0x1010` together make channel 0 (RGB and A); `0x100F` and `0x1011` make
channel 1. **Colour and alpha are fully independent computations** — own enable, sources, mask,
diffuse and attenuation — sharing only the light blocks (`gx.h:1851-1854`).

### A.2.1 A libogc defect the test program must work around

`GX_SetChanCtrl()` continues, `gx.c:2198-2208`:

```c
reg = (channel&0x03);
__gx->chnCntrl[reg] = val;
__gx->dirtyState |= (0x1000<<reg);

if(channel==GX_COLOR0A0) {
    __gx->chnCntrl[2] = val;
    __gx->dirtyState |= 0x5000;
} else {
    __gx->chnCntrl[3] = val;
    __gx->dirtyState |= 0xa000;
}
```

With `GX_COLOR0` = 0, `GX_COLOR1` = 1, `GX_ALPHA0` = 2, `GX_ALPHA1` = 3, `GX_COLOR0A0` = 4,
`GX_COLOR1A1` = 5 (`gx.h:44-49`), the `else` branch runs for **every channel except
`GX_COLOR0A0`**, so a call for `GX_COLOR0`, `GX_COLOR1` or `GX_ALPHA0` **also overwrites the
`GX_ALPHA1` register (`0x1011`)** with the same word. `GX_COLOR0A0` and `GX_COLOR1A1` behave as
documented. Consequence for a test that wants channel 0's colour and alpha configured differently:
call `GX_SetChanCtrl(GX_COLOR0A0, …)` first, then `GX_SetChanCtrl(GX_ALPHA0, …)`, then set channel 1
last — or bypass the API with direct `0x10` writes. Writes are lazy: the shadow words reach the FIFO
at the next draw, via `__GX_SetDirtyState()` (`gx.c:1042-1047`).

## A.3 Ambient and material colours (`0x100A`–`0x100D`)

RGBA8 with **red in bits 31:24**, alpha in 7:0 (`GX_SetChanAmbColor()`, `gx.c:2211-2245`;
`GX_SetChanMatColor()`, `gx.c:2247-2281`). The `GX_COLORn` / `GX_ALPHAn` variants of those two
functions preserve the other part of the word; the `…A…` variants write all four bytes. `GX_Init()`
leaves every channel disabled, ambient source register, material source **vertex**, ambient
`{0,0,0,0}` and material `{255,255,255,255}` (`gx.c:653-659`).

## A.4 Channel count and vertex specs

* **`0x1009`** = number of lit colour channels passed on to the rasteriser, 0–2; libogc copies it
  from `genMode` bits 6:4 (`gx.c:1010`, `GX_SetNumChans()` `gx.c:3982-3986`).
* **`0x1008`** vertex specs, packed at `gx.c:775`:
  `(_SHIFTL(texs,4,4))|(_SHIFTL(nrms,2,2))|(cols&0x3)` — bits 1:0 colour count, bits 3:2 normal
  count (0 = none, 1 = normal, 2 = normal + binormal + tangent), bits 7:4 texcoord count.
  **Unverified:** whether the XF gates lighting on bits 3:2, and what normal it uses for a lit
  vertex that carries none (the runtime substitutes `(0,0,1)`, `gc_gx_draw.c:489`).

Vertex colour routing, per `gx.h:1856-1858`: if exactly one of `GX_VA_CLR0` / `GX_VA_CLR1` is
present, that colour feeds **channel 0**; channel 1 sees a vertex colour only when both are present.
**Unverified** for hardware; the runtime keeps `CLR1` in slot 1 regardless (`gc_gx.c:442-447`), so a
case with only `CLR1` present discriminates.

## A.5 The light block (`0x0600 + 16·n`)

`GX_LoadLightObj()` emits a 16-word burst at `0x600|(_SHIFTL(id,4,8))` (`gx.c:4623-4640`), where `id`
is recovered from the one-hot `GX_LIGHTn` mask by a switch (`gx.c:4593-4621`; an invalid mask maps
to light 0). Field order is `struct __gx_litobj` (`gx_regdef.h:85-101`):

| Word | Offset from block | Field | Type | Set by |
|---|---|---|---|---|
| 0–2 | `+0x0`–`+0x2` | Reserved, written as 0 | u32 | `gx.c:4625-4627` |
| 3 | `+0x3` | Colour, RGBA8, red in 31:24 | u32 | `GX_InitLightColor()` `gx.c:4581-4585` |
| 4–6 | `+0x4`–`+0x6` | `a0`, `a1`, `a2` — cosine (angular) attenuation coefficients | f32 | `GX_InitLightAttn()` `gx.c:4732-4742`, `…AttnA()` `4744-4751`, `GX_InitLightSpot()` `4808-4863` |
| 7–9 | `+0x7`–`+0x9` | `k0`, `k1`, `k2` — distance attenuation coefficients | f32 | `GX_InitLightAttn()`, `…AttnK()` `4753-4760`, `GX_InitLightDistAttn()` `4695-4730` |
| 10–12 | `+0xA`–`+0xC` | Position x, y, z | f32 | `GX_InitLightPos()` `gx.c:4572-4579`; specular helpers |
| 13–15 | `+0xD`–`+0xF` | Direction x, y, z — **or the half-angle vector for a specular light** | f32 | `GX_InitLightDir()` `gx.c:4686-4693`; specular helpers |

Facts about the stored values that a test author must hold on to:

* **Position and direction are in view (eye) space and the XF applies no matrix to them.**
  `gx.h:4667-4668` requires the position to be "in the same space as a transformed vertex position
  (i.e., view space)"; `gx.h:4706-4707` requires the direction to be consistent with a normal after
  the normal matrix. Neither sentence says outright that the hardware does not transform them, but
  both would be meaningless if it did. Case F.19 checks the position half.
* **The direction is stored negated.** `GX_InitLightDir()` writes `-(nx)`, `-(ny)`, `-(nz)`
  (`gx.c:4690-4692`). The API argument is the way the light points; the stored vector points back
  towards the light. Nothing is normalised (`gx.c:4686-4693`).
* **Specular lights overload position and direction** (`GX_InitSpecularDir()`, `gx.c:4779-4806`;
  `GX_InitSpecularDirHA()`, `gx.c:4762-4777`): position ← direction × `LARGE_NUMBER`, which is
  `-1048576.0f` on GameCube (`gx.c:22-26`), placing the light effectively at infinity on the lit side;
  direction ← the half-angle vector, `normalize(−n + (0,0,1))` for `GX_InitSpecularDir()` or
  caller-supplied for `…HA()`. `gx.h:4913-4918` forbids `GX_InitLightDir()` / `GX_InitLightPos()` on
  a specular light for exactly this reason, and calls a specular light parallel.
* **`GX_LoadLightObjIdx()`** is the indexed alternative: FIFO command `0x38` with word
  `(litobjidx << 16) | 0xF600 | (id << 4)` (`gx.c:4643-4684`) — count nibble `0xF` = 16 words.

## A.6 Normal matrices and the normal index

* `GX_LoadNrmMtxImm(mt, pnidx)` writes **9 words at `0x0400 + 3·pnidx`** (`gx.c:2884-2888`),
  taken from the upper-left 3×3 of a 3×4 `Mtx`, row-major, translation column dropped
  (`WriteMtxPS3x3from4x3()`, `gx.c:2812-2832`: offsets 0, 4, 8, 16, 20, 24, 32, 36, 40).
  `GX_LoadNrmMtxImm3x3()` is the same from a 3×3 (`gx.c:2890-2894`).
* `pnidx` values are `GX_PNMTX0..9` = 0, 3, … 27 (`gx.h:210-219`), so libogc's normal matrices
  land at `0x400 + 9k`. `GX_SetCurrentMtx()` puts the same index in `0x1018` bits 5:0
  (`gx.c:2931-2935`).
* **libogc never computes a normal matrix.** The application supplies it; `gx.h:2369-2372` and
  `gx.h:2416-2418` say it should "generally" be the inverse transpose of the modelview. A test can
  therefore load *any* 3×3 — including a scaled or a deliberately wrong one — and that is how §D's
  cases are built.
* **Which address a non-multiple-of-3 index selects is Unverified.** libogc's formula gives
  `0x400 + 3·index` for any index; Dolphin behaviour is `0x400 + 3·(index & 31)`
  (`TransformUnit.cpp:99`); **the runtime uses `0x400 + 9·(index / 3)`** (`gc_gx_draw.c:499-500`),
  which agrees for multiples of 3 and differs otherwise. Case F.18 discriminates.

---

# Part B — The per-vertex lighting equation

Notation, per channel component (R, G, B or A), all vectors in view space:

* `P` — the vertex position after the position matrix.
* `N` — the vertex normal after the normal matrix (whether renormalised: Part D).
* For light *i*: `C` its colour byte for this component, `Lp` position words, `Ld` direction words
  (as stored, i.e. already negated), `a0..a2`, `k0..k2`.
* `V = Lp − P`, `d = |V|`, `L = V / d`.

## B.1 Channel output

```
if enable == 0:   out = mat                                        (gx.h:1846-1848)
else:             illum = amb + Σ_{i in mask} contribution_i
                  I     = clamp(quantise(illum), 0, 255)
                  out   = (mat * (I + (I >> 7))) >> 8               (§C.2 — silicon-verified)
```

`mat` and `amb` are the register byte or the vertex colour byte according to bits 0 and 6.

* **The ambient is the accumulator's starting value**, not a term multiplied separately. Established
  by the hardware test: enabled channel, empty mask, output = material × widened ambient
  (`lighting.cpp:22-33, 86`).
* **The material multiplies once, after the lights are summed with the ambient.** This is the
  structure every implementation read uses and is consistent with `gx.h:4797-4799` (attenuation
  scales the *light* colour, with no mention of material), but **Unverified:** no source separates
  "material × (ambient + Σ lights)" from, say, per-light material products that happen to agree at
  empty mask. The two differ once the sum saturates: material 128 (register), ambient 200, one light
  (attn none, diffuse none) of colour 200 gives `I = 255` and `(128·256) >> 8 = `**128** under the
  single multiply, but about 200 if ambient and light were modulated separately and then added. Add
  it to the run as F.23d.
* `quantise` and the position of the clamp are **Unverified** — §C.3 and §C.4.
* **Unverified:** accumulation order over the mask. Ascending light index is what every
  implementation read does; it only matters if the clamp is per-light (§C.4).

## B.2 One light's contribution

```
contribution = C × diffuse × atten
```

That diffuse and attenuation are **multiplied**, both scaling the light colour, is stated by
`gx.h:4797-4799` ("the light color will be multiplied by the atten factor") and by
`gx.h:1844-1846` (both functions control "the lighting equation for all lights"). Case F.4 checks
the product directly.

## B.3 Diffuse function (bits 8:7)

| Value | Name | `diffuse` |
|---|---|---|
| 0 | `GX_DF_NONE` | `1` |
| 1 | `GX_DF_SIGNED` | `N · L` — may be negative, so a back-facing light **subtracts** |
| 2 | `GX_DF_CLAMP` | `max(N · L, 0)` |
| 3 | — | **Unverified.** Never written by libogc. |

Enum values `gx.h:193-195`. The semantics of "signed" vs "clamp" are from the names; the header
gives no formula. Dolphin behaviour (both backends) is exactly the table above, with `L` the
**normalised** vector from vertex to the light *position* words (`TransformUnit.cpp:269-283`).
**Unverified:** that hardware normalises `L` — no source states it, but without it `N·L` would scale
with distance, which no documented light model does.

## B.4 Attenuation function (bits 10:9)

`gx.h:4794` gives one closed form for the non-trivial cases. Written out, with the header's typo
corrected (see below):

```
             max(0, a0 + a1·u + a2·u²)
    atten = ---------------------------
                k0 + k1·v + k2·v²
```

**The polynomials are in ascending power order: coefficient 0 is the constant term.** Both the
header's ordering and every libogc helper confirm it — `GX_InitLightDistAttn()` always sets `k0 = 1`
as "off" (`gx.c:4719-4724`), which only makes sense as the constant term; `GX_InitLightSpot()`'s
`GX_SP_OFF` sets `a0 = 1, a1 = a2 = 0` (`gx.c:4852-4856`).

**`a2` is not squared.** The header prints the leading term as `a2^2 * aattn^2`; the libogc
coefficient sets are only correct with `a2` to the first power (derivation in
`gc-lighting-texgen.md` §A.4.1 — e.g. `GX_SP_COS2` gives exactly 1 on the axis only unsquared).

**The clamp is on the numerator only**, as `clamp0(…)` in `gx.h:4794`. The denominator is unclamped,
so a negative denominator gives a negative `atten`. Case F.10.

### B.4.1 Value 2 (and presumably 0) — none

`atten = 1`. The six coefficients are ignored. Dolphin behaviour, both backends
(`LightingShaderGen.cpp:25-30`, `TransformUnit.cpp:224-231`). The header implies it by specifying the
ratio only "when the attenuation function … is `GX_AF_SPOT`" (`gx.h:4796-4799`). Case F.2.

### B.4.2 Value 3 — spot

Per `gx.h:4796-4798`:

* `u` = the cosine of the angle between the light's direction and the vector from the light to the
  vertex. With stored (negated) direction words that is **`u = L · Ld`** — both vectors pointing
  back towards the light, so a vertex on the axis gives `u = +1`.
* `v` = **`d`**, the Euclidean distance from light position to vertex. Not `d²`; `d²` appears only
  as `v²` in the `k2` term.

So the numerator is a quadratic in a cosine and the denominator a quadratic in a length. Cases F.5,
F.6, F.7.

**Unverified — whether `u` is clamped at zero before the polynomial.** Dolphin behaviour (both
backends) is `u = max(0, L·Ld)` (`TransformUnit.cpp:250`, `LightingShaderGen.cpp:43-52`); the header
says nothing; **the runtime does not clamp** (`gc_gx_draw.c:176`). It matters whenever `a0 > 0` or
`a1 < 0` (for example `GX_SP_RING2`, whose `a0` is positive). Case F.8.

### B.4.3 Value 1 — specular

Per `gx.h:4807-4808`, **`u` and `v` are the same quantity: `N · H`**, the dot of the eye-space normal
with the half-angle vector held in the direction words:

```
    s     = N · Ld                       (Ld holds H for a specular light)
    atten = max(0, a0 + a1·s + a2·s²) / (k0 + k1·s + k2·s²)
```

and it approximates `(N·H)^shininess` (`gx.h:5308-5312`). The distance is not used; nor is the
cosine to the light position. `GX_InitLightShininess(lobj, s)` is
`GX_InitLightAttn(lobj, 0, 0, 1, s/2, 0, 1 − s/2)` (`gx.h:5319-5322`), i.e.
`atten = c² / (s/2 + (1 − s/2)·c²)` with `c = N·H`, which is 1 at `c = 1` for every `s`.

Three further behaviours are **Dolphin behaviour only, Unverified for hardware, and not implemented
by the runtime** (`gc_gx_draw.c:170-173, 216-219`):

1. **Back-face gate.** If `N · L < 0`, where `L` comes from the *position* words, `s` is forced to
   0 before the polynomials (`TransformUnit.cpp:234-235`). Note this zeroes `s`, not the
   contribution: the result is `max(0,a0)/k0`, which is 0 for the shininess macro. Case F.13.
2. **`s` clamped at zero** — `s = max(0, N·H)` (same line). Case F.14.
3. **`k` normalised when the diffuse field is non-zero.** In specular mode, if bits 8:7 ≠ 0, the
   three `k` coefficients are scaled to unit length as a vector before use
   (`TransformUnit.cpp:239-240`; the shader generator does the same). libogc can never produce this
   state (§A.2); a direct register write can. It is so specific that it is presumably copied from
   some observation, but nothing Dolphin ships documents one. Case F.15.

In specular mode the diffuse factor is still whatever bits 8:7 select (1 for libogc titles). With a
non-zero field, Dolphin computes `N · L` from the position words — which, for a light built by
`GX_InitSpecularDir()`, is the light direction at infinity.

## B.5 Degenerate inputs

None of these is specified by any source. Implementations differ, so each is a Part F
**record-only** case: run it on hardware, write the number down, then choose.

| Situation | Runtime | Dolphin-SW | Dolphin-HW |
|---|---|---|---|
| Denominator exactly 0 | `atten = 0` (`gc_gx_draw.c:189`) | `atten = 1` if numerator > 0, else 0 (`TransformUnit.cpp:211-214`) | if all three `k` are below 1e-5 in magnitude, `k0` is replaced by 1e-5 on upload (`VertexShaderManager.cpp:239-248`), giving a very large ratio; otherwise IEEE division |
| Vertex exactly at the light position (`d = 0`) | `L` left as the zero vector, so `N·L = 0` (`gc_gx_draw.c:203-206`) | attempts to substitute `N` for `L` when the normalised vector is zero (`TransformUnit.cpp:227-229`); whether that fires depends on how its normalise treats a zero vector | same intent (`LightingShaderGen.cpp:29`) |
| Non-unit direction words | used as stored | used as stored | **normalised on upload**, with NaN → 0 and ±inf → ±1 (`VertexShaderManager.cpp:256-271`); a comment there says hardware testing is needed |

libogc reaches the zero-denominator case through `GX_InitLightShininess(lobj, 0)` (`k = (0,0,1)`,
zero at `N·H = 0`) and through `GX_InitLightAttn()` with `k0 = 0`, and writes non-finite
coefficients from `GX_InitLightSpot(…, 0.0f, …)` and `GX_InitLightDistAttn()` with a zero reference
distance or brightness (`gc-lighting-texgen.md` §A.5).

---

# Part C — Precision, clamping and quantisation

## C.1 What the inputs are

* **Colours are 8-bit unsigned per component**: light colour word (`gx.c:4584`), ambient and
  material registers (`gx.c:2213`, `gx.c:2249`), and vertex colours after the vertex loader.
* **Coefficients, positions, directions and matrices enter the XF as IEEE-754 single-precision
  words** (`wgPipe->F32 = …`, `gx.c:4629-4640`; the matrix writers at `gx.c:2812-2860`).
* **Unverified: the precision the XF computes in.** The only hint is the hardware test's own comment
  (`lighting.cpp:48-51`), whose author attributes the non-obvious final rounding to the hardware using
  fixed-point arithmetic with shortcuts. That is an assertion by a test writer, not a measurement,
  and it describes only the last step. `gx.h:1059` names a performance counter for the number of
  cycles "the lighting engine is busy", which suggests a sequential (iterated) datapath rather than
  parallel float units — also not evidence of a number format.

## C.2 The final multiply — silicon-verified

```
out = (mat * (I + (I >> 7))) >> 8          mat, I ∈ [0, 255]
```

Established by `lighting.cpp:52-88`: lighting enabled, both sources from the register, empty mask
(the control word is zeroed then only the enable bit set, `lighting.cpp:26-33`), all 65,536
ambient × material pairs, red channel of colour channel 0 read back from an RGB8 EFB
(`lighting.cpp:43`) after TEV passes the rasterised colour through (`lighting.cpp:78`). The runtime
implements it exactly (`gc_gx_draw.c:238-241`) and passes all 65,536.

Properties the Part F expected values rely on:

* **With `mat = 255`, `out = I` for `I ≥ 128` and `out = I − 1` for `1 ≤ I ≤ 127`**, `out = 0` for
  `I = 0`. Every Part F case uses `mat = 255` so the output reads back the illumination almost
  directly; where `I ≥ 128` it is exact.
* The widening is applied to the illumination only; there is no rounding term.

**What that test does not cover:** green, blue, alpha; channel 1; vertex-sourced material or ambient;
any light; any value of `I` that did not come straight from an 8-bit register. In particular it does
not show that `I` is an integer before the multiply — only that when it is, this is the rounding.

## C.3 How a light's fractional contribution becomes an integer — Unverified

The three implementations read make three different choices, and all three agree with §C.2:

| Implementation | Accumulate in | Convert to integer | Clamp |
|---|---|---|---|
| Runtime | float | **once**, round half up (`(int)(x + 0.5f)`) — `gc_gx_draw.c:243-247` | once, after |
| Dolphin-SW | float | **once**, truncate toward zero (`static_cast<int>`) — `TransformUnit.cpp:355-357, 389` | once, after |
| Dolphin-HW | integer | **per light**, `round()` of each light's product, then integer add — `LightingShaderGen.cpp:60-69` | once, after all lights (`:149`) |

These are distinguishable with lights whose contributions are exact binary fractions (case F.20).
Hardware may do none of the three — for instance, carry a fixed number of fraction bits through the
sum and truncate.

## C.4 Where the clamps are

| Clamp | Status |
|---|---|
| Numerator `max(0, …)` | **Documented** — `gx.h:4794` |
| Denominator | **None**, per `gx.h:4794` (only the numerator is wrapped) |
| Diffuse `max(0, N·L)` for `GX_DF_CLAMP` | From the name; all implementations agree |
| Spot cosine `u ≥ 0` | **Unverified** — Dolphin yes, runtime no (F.8) |
| Specular `N·H ≥ 0` and `N·L ≥ 0` gate | **Unverified** — Dolphin yes, runtime no (F.13, F.14) |
| Illumination to `[0, 255]` before the final multiply | **Must exist** — `I + (I>>7)` is only an 8-to-9-bit widen for `I ≤ 255`, and the hardware test's `I` values are all in range, so this is inference. **Unverified:** whether it is applied once after all lights or after each add. All three implementations clamp once. Case F.21 discriminates. |
| Per-light contribution clamped before adding | **Unverified**; no implementation does it. A signed diffuse would be meaningless if contributions were clamped at 0 individually, which is weak evidence against. |

---

# Part D — Normals

* **Transform.** `N = M · n`, with `M` the 3×3 at the normal-matrix address for the current
  position index (§A.6), row-major: `N.x = m00·n.x + m01·n.y + m02·n.z`, etc. Row-major follows from
  `WriteMtxPS3x3from4x3()` emitting row 0 first (`gx.c:2812-2832`) and from position matrices being
  applied the same way. No translation. Case F.18b checks the multiplication order.
* **Renormalisation — Unverified.** No libogc source addresses it. Dolphin behaviour normalises the
  transformed normal used for lighting in both backends (`TransformUnit.cpp:105-109`,
  `VertexShaderGen.cpp:902`), with a comment explaining the motive: games scale the normal matrix to
  control emboss-map strength (the binormal and tangent are left unnormalised) and still need unit
  normals for lighting. The runtime also normalises (`gc_gx_draw.c:510-513`). This is a plausible
  story about game content, not a hardware observation. Cases F.17a/b settle it with a factor-of-two
  difference.
* **Un-normalised input normals.** If hardware renormalises after the matrix, input length is
  irrelevant (except zero). If it does not, `N·L` and `N·H` scale linearly with length, and so does
  every diffuse contribution — a normal of length 2 doubles a clamped-diffuse light. Quantised normals
  (`GX_S8` with 6 fraction bits, say) are typically within 1% of unit length, too close to detect
  reliably through 8-bit output; F.17 uses lengths 2 and 0.5.
* **Zero-length normal** after the matrix: **Unverified.** The runtime leaves it zero
  (`gc_gx_draw.c:511`), giving `N·L = 0`.
* **Light vectors are not transformed** by the normal matrix (§A.5). Case F.18b rotates the normal
  and not the light.

---

# Part E — libogc calls a test program will make

| Goal | Call | XF effect |
|---|---|---|
| Output one lit channel | `GX_SetNumChans(1)` | `0x1009 = 1` at next draw (`gx.c:1010, 3982-3986`) |
| Configure a channel | `GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE, ambsrc, matsrc, GX_LIGHT0\|…, diff, attn)` | `0x100E` and `0x1010` ← word of §A.2 (mind §A.2.1) |
| Ambient / material | `GX_SetChanAmbColor(GX_COLOR0A0, c)`, `GX_SetChanMatColor(GX_COLOR0A0, c)` | `0x100A` / `0x100C` ← `RRGGBBAA` |
| Light colour | `GX_InitLightColor(&lo, c)` | word 3 on load |
| Local light position | `GX_InitLightPos(&lo, x, y, z)` — view space | words 10–12 on load, verbatim |
| Spot direction | `GX_InitLightDir(&lo, dx, dy, dz)` — the way the light points | words 13–15 ← `(−dx, −dy, −dz)` |
| Exact coefficients | `GX_InitLightAttn(&lo, a0, a1, a2, k0, k1, k2)` | words 4–9, verbatim, same order |
| Specular light, auto half-angle | `GX_InitSpecularDir(&lo, nx, ny, nz)` | words 10–12 ← `n × −1048576`; 13–15 ← `normalize(−n + (0,0,1))` |
| Specular light, explicit H | `GX_InitSpecularDirHA(&lo, nx, ny, nz, hx, hy, hz)` | words 10–12 ← `n × −1048576`; 13–15 ← `h` verbatim |
| Shininess | `GX_InitLightShininess(&lo, s)` | `a = (0,0,1)`, `k = (s/2, 0, 1 − s/2)` |
| Upload | `GX_LoadLightObj(&lo, GX_LIGHTn)` | 16 words at `0x600 + 16n` |
| Normal matrix | `GX_LoadNrmMtxImm(mt, GX_PNMTX0)` / `…Imm3x3` | 9 words at `0x400 + 3·pnidx` |
| Position matrix | `GX_LoadPosMtxImm(mt, GX_PNMTX0)`; `GX_SetCurrentMtx(GX_PNMTX0)` | 12 words at `4·pnidx`; `0x1018` bits 5:0 |

Rules of use:

* **Use `GX_InitLightAttn()` with literal coefficients** in tests, never `GX_InitLightSpot()` /
  `GX_InitLightDistAttn()`. The helpers run on the CPU, so they test libogc, not the XF — and
  `GX_SP_SHARP` is wrong in libogc (`gx.c:4836`; `gc-lighting-texgen.md` §A.5.1).
* A `GXLightObj` is 16 opaque words (`gx.h:1266-1268`); initialise every field before loading — a
  stack object with stale `a`/`k` words is a silent source of wrong numbers.
* `GX_SetChanCtrl()` rewrites the attenuation enum and forces `GX_DF_NONE` with `GX_AF_SPEC`; for
  F.15 and F.25 write the control word directly.

---

# Part F — Test cases

## F.0 Harness

The measurable quantity is a lit vertex colour, read back as an 8-bit EFB value. To read one
vertex's result exactly, the pixel sampled must not be an interpolation of different vertex colours.
Lighting depends on vertex position, so the corners of a quad generally light differently. Two ways
round it:

1. **Preferred: `GX_POINTS`** with `GX_SetPointSize()` (`gx.c:3768`) large enough to cover several
   pixels. One vertex, one colour, no interpolation.
2. A quad centred on the light's axis with the normal along that axis — all four corners are then
   equidistant and at the same angle. Only works for cases whose light lies on the axis.

Common setup, unless a case says otherwise:

* Orthographic projection whose depth range contains view-space `z = −5`; position matrix
  identity at `GX_PNMTX0`; normal matrix identity at `GX_PNMTX0`.
* Vertex at view space **`P = (0, 0, −5)`**, normal **`n = (0, 0, 1)`**, no vertex colour.
* `GX_SetNumChans(1)`; channel 0 lit; ambient and material from the register;
  **material = 255**; the light's G, B and A bytes 0.
* TEV stage 0 passes `GX_CC_RASC` (or `GX_CC_RASA` for F.22) to the output; RGB8 pixel format, as
  the existing test uses (`lighting.cpp:43, 78`).
* To draw several cases in one frame, give each point its own screen position **and translate that
  case's light position by the same offset**; with small integer offsets the difference `Lp − P` is
  bit-identical.
* Positions below are view-space; "light at `P + (x,y,z)`" means `GX_InitLightPos()` with that sum.
  Directions are given **as stored** (`Ld`); the `GX_InitLightDir()` argument is their negation.

Expected values were computed with a float32 model of §B (script in the session scratchpad, not in
the repository) and are the red output byte. **Compare with a tolerance of ±1** except in F.20 and
F.21, which use exact binary fractions and must match exactly. Every wrong variant listed differs
from the reference by at least 3, and almost all by 20 or more.

Columns: **Ref** is the result of §B's documented equation; where §B leaves a choice open the case
is marked *record* and the three implementations are listed instead. **RT** is what the runtime is
predicted to produce by reading `gc_gx_draw.c` (not by running it). **D-HW / D-SW** are Dolphin's
two backends.

## F.1 Structure and register decoding

| # | Purpose | Setup | Ref | Wrong-implementation results |
|---|---|---|---|---|
| F.1 | Light-mask bit positions | Load all eight lights, colour R = `10·(k+1)`, attn none, diffuse none, amb 128. Run once per k with only light k enabled (register bit `2+k` or `7+k`). | `138 + 10k` | high nibble ignored → 128 for k ≥ 4; high nibble taken from bits 9:6 → bits 6–9 (ambient source, diffuse, attenuation) masquerade as light enables and the wrong lights respond |
| F.2 | Attenuation "none" ignores `a`, `k` | amb 128; colour 96; `a = (0.25,0,0)`, `k = (4,0,0)`; light at `P+(0,0,10)`; diffuse none; attn value 2 | **224** | ratio applied anyway → 134 |
| F.3a | Diffuse none | amb 128; colour 90; light at `P+(8,0,−6)` (so `N·L = −0.6`); attn none; diffuse 0 | **218** | — |
| F.3b | Diffuse signed | as F.3a, diffuse 1 | **73** | clamped → 128; ignored → 218 |
| F.3c | Diffuse clamp | as F.3a, diffuse 2 | **128** | missing clamp → 73 |
| F.4 | Diffuse × attenuation is a product | colour 213; light at `P+(0,6,8)` (`N·L = 0.8`); `Ld = (0,0,1)`; `a = (0,1,0)`, `k = (1,0,0)` (so `atten = u = 0.8`); diffuse clamp; attn spot | **136** | attn none (F.4 control) → 170; sum instead of product → 255 |
| F.22 | Alpha channel uses the light's alpha byte and its own control word | `GX_COLOR0A0` both lit, attn none, diffuse none; light colour `(30,0,0,150)`; ambient `(0,0,0,64)`; read R via `RASC` and A via `RASA` | R **29**, A **214** | alpha fed from R byte → 93; alpha channel unlit → 255 |
| F.23a | Ambient from vertex | vertex colour R = 100; ambsrc vertex, matsrc register (255); empty mask | **99** | ambient taken from register (0) → 0 |
| F.23b | Both from vertex | vertex colour R = 100; both sources vertex; empty mask | **39** | material not applied → 99 |
| F.23d | Material applied once, after the sum saturates | material 128 (register); amb 200; one light colour 200 at `P+(0,0,10)`; attn none; diffuse none | **128** | material applied to ambient and light separately, then summed → 200 |
| F.23c | Only `GX_VA_CLR1` present, channel 0 material from vertex | vertex `CLR1` R = 100, no `CLR0`; channel 0 lighting disabled, matsrc vertex | *record* — `gx.h:1856-1858` predicts **100** | RT keeps it in channel 1 → channel 0 falls back to the register (255) |

## F.2 The spot and distance polynomials

Geometry for F.5–F.7 and F.16: light at **`P + (−6, 0, 8)`**, so `d = 10` and
`L = (−0.6, 0, 0.8)`; `Ld = (0, 0, 1)`, so `u = 0.8`. Attn spot, diffuse none, amb 0.

| # | Purpose | Coefficients / change | Ref | Wrong-implementation results |
|---|---|---|---|---|
| F.5 | Direction sign: stored `Ld` used as-is, `L` points vertex → light | colour 200; `a = (0,1,0)`, `k = (1,0,0)` | **160** | direction negated again, or `L` taken light → vertex → 0 |
| F.6 | `a` order, `a2` not squared | colour 200; `a = (0.25, 0, 0.75)`, `k = (1,0,0)` | **146** | `a` reversed (`a2` as constant) → 182; `a2` squared → 121 |
| F.7 | `k` order, `v = d`, distance not cosine | colour 200; `a = (1,0,0)`, `k = (0.5, 0.1, 0.01)` | **79** | `k` reversed → 3; cosine fed to `k` (the spot/specular mix-up) → 255; `d²` used as `v` → 1 |
| F.8 | Spot cosine clamp at zero | colour 200; light at `P+(−6,0,−8)` (so `u = −0.8`); `a = (0.5, −0.5, 0)` | *record*: D-HW **99**, D-SW **99**, RT **180** | — |
| F.9 | Numerator clamp | amb 128; colour 90; light at `P+(0,0,10)`; `Ld = (0.96, 0, 0.28)` (`u = 0.28`); `a = (−0.5, 1, 0)` (numerator −0.22) | **128** | numerator unclamped → 107 |
| F.10 | Denominator not clamped | amb 128; colour 90; light at `P+(0,0,10)`, `Ld = (0,0,1)`; `a = (0.5,0,0)`, `k = (−1,0,0)` | **82** | `|denominator|` → 173; denominator clamped to 0 → falls into F.11 |
| F.11 | Zero denominator | amb 64; colour 100; as F.10 with `a = (1,0,0)`, `k = (0,0,0)` | *record*: RT **63**, D-SW **164**, D-HW **255** | — |
| F.16 | Direction words not normalised | colour 90; `a = (0,1,0)`, `k = (1,0,0)`; `Ld = (0,0,2)` (so raw `u = 1.6`) | *record*: RT **144**, D-SW **144**, D-HW **71** | — |
| F.19 | Distance measured in view space; light not transformed | colour 200; position matrix = translate `(0,0,−5)`, vertex at model origin; light at `(0,0,−3)`; `a = (1,0,0)`, `k = (0,1,0)` (so `atten = 1/d`) | **99** (`d = 2`) | model-space vertex, or light also transformed by the modelview (`d = 3`) → 66 |

## F.3 Specular

All specular cases: attn value 1, diffuse field 0 unless stated, amb 0. The light is built with
`GX_InitSpecularDirHA()`, giving position words `n × −1048576` and direction words `H`.

| # | Purpose | Setup | Ref | Wrong-implementation results |
|---|---|---|---|---|
| F.12 | `s = N·H` from the direction words | normal `(0, 0.6, 0.8)`; position words `(0,0,+1048576)`; `H = (0.6, 0, 0.8)`; colour 205; `a = (0,1,0)`, `k = (1,0,0)` | **131** (`N·H = 0.64`) | `N·L` from position words (0.8) → 164; spot formula (`L·Ld`, distance) → 164 |
| F.12b | Denominator also in `N·H`, not distance | as F.12, colour 203, `k = (0.5, 0, 0.5)` | **184** | spot formula (`v = d ≈ 10⁶`) → 0 |
| F.15b | Shininess macro end to end | normal `(0,0,1)`; `H = (0.6,0,0.8)` (`c = 0.8`); colour 200; `GX_InitLightShininess(&lo, 16)` → `a = (0,0,1)`, `k = (8,0,−7)` | **35** | exponent evaluated as a real power `0.8¹⁶` → 5; `k` ignored → 128 |
| F.13 | Back-face gate on `N·L` | normal `(0,0,1)`; position words `(0,0,−1048576)` (light behind); `H = (0.6,0,0.8)`; colour 200; `a = (0,1,0)`, `k = (1,0,0)` | *record*: D-HW **0**, D-SW **0**, RT **160** | — |
| F.14 | `N·H` clamped at zero | normal `(0,0,1)`; position words `(0,0,+1048576)`; `H = (0.6, 0, −0.8)`; colour 91; `a = (1,0,1)`, `k = (1,0,0)` | *record*: D-HW **90**, D-SW **90**, RT **149** | — |
| F.15 | `k` normalisation with a non-zero diffuse field (direct register write) | control word with attn 1 **and** diffuse 2; normal `(0,0,1)`; position words `(0,0,+1048576)`; `H = (0.6,0,0.8)`; colour 200; `a = (0,0,1)`, `k = (0,0,2)` | *record*: D-HW **200**, D-SW **200**, RT **99** | — |

## F.4 Normals

| # | Purpose | Setup | Ref | Wrong-implementation results |
|---|---|---|---|---|
| F.17a | Renormalisation, long normal | vertex normal `(0,0,2)`; colour 90; light at `P+(0,0,10)`; diffuse clamp; attn none | *record*: renormalised **89** (RT, D-HW, D-SW) | not renormalised → 180 |
| F.17b | Renormalisation, short normal | vertex normal `(0,0,0.5)`; amb 128; otherwise F.17a | *record*: renormalised **218** | not renormalised → 173 |
| F.18 | Normal-matrix address for a position index that is not a multiple of 3 | `GX_LoadNrmMtxImm3x3(identity, 0)` then `GX_LoadNrmMtxImm3x3(M, 1)` with rows `M = [(0,1,0),(0,0,1),(0,0,−1)]` — words `0x400..0x40B` then read `1,0,0, 0,1,0, 0,0,1, 0,0,−1`; valid position matrices at index 0 and 1; draw the index-0 control **before** loading the index-1 position matrix (position slots overlap: index 1 is words `0x004`–`0x00F`, which rewrites rows 1–2 of index 0), then load it and draw with `GX_SetCurrentMtx(1)`; colour 200; light at `P+(0,0,10)`; diffuse clamp; attn none | index 0: **200** both ways. Index 1: `0x403` (libogc formula, D-HW, D-SW) → **0** | RT reads `0x400` for index 1 → 200 |
| F.18b | Normal matrix applied, row-major, light not rotated | normal matrix rows `[(1,0,0),(0,0,−1),(0,1,0)]`; vertex normal `(0,1,0)` (→ `(0,0,1)`); colour 90; light at `P+(0,0,10)`; diffuse clamp; attn none | **89** | matrix transposed (→ `(0,0,−1)`) → 0; matrix not applied → 0; light direction also rotated → differs |

## F.5 Accumulation, rounding and clamping

These use exact binary fractions so that the three conversions of §C.3 separate cleanly. **Match
exactly.** Setup: amb 200; material 255; attn spot; diffuse none; each light colour R = **1**,
light at `P+(0,0,10)`, `Ld = (0,0,1)`, `k = (1,0,0)`, `a = (f, 0, 0)` so each light contributes
exactly `f`.

| # | Lights | Exact sum | Round once (RT) | Truncate once (D-SW) | Round per light (D-HW) |
|---|---|---|---|---|---|
| F.20a | one, `f = 0.625` | 200.625 | **201** | **200** | **201** |
| F.20b | two, `f = 0.375` each | 200.75 | **201** | **200** | **200** |
| F.20c | three, `f = 0.375` each | 201.125 | **201** | **201** | **200** |

The triple (F.20a, b, c) is `(201,201,201)`, `(200,200,201)` or `(201,200,200)` for the three models;
any other triple means hardware does something else (for instance a fixed number of fraction bits),
and more fractions — 1/16 steps are exact — will locate it.

| # | Purpose | Setup | Clamp once at end (all three implementations) | Saturate after each light, ascending | Saturate after each, descending |
|---|---|---|---|---|---|
| F.21 | Clamp position and order | amb 100; diffuse **signed**; attn none; light 0 colour 200 at `P+(0,0,−10)` (`N·L = −1`); light 1 colour 200 at `P+(0,0,10)` (`N·L = +1`) | **99** | **200** | **54** |

## F.6 Record-only register states

No source defines these; run them so the emulator can copy hardware rather than guess.

| # | State | Suggested setup |
|---|---|---|
| F.24 | Vertex exactly at the light position | light at `P`; colour 200; diffuse clamp; attn none. RT gives 0; Dolphin's fallback intends 200. |
| F.25a | Attenuation field value 0 | F.2 with the control word's bits 10:9 = 0. Output 224 means "none"; 134 means the ratio is evaluated. |
| F.25b | Diffuse field value 3 | F.3a geometry with bits 8:7 = 3. Compare with 218 / 73 / 128. |
| F.26 | Lit vertex with no normal in the vertex format | F.17a's light (colour 90 at `P+(0,0,10)`, diffuse clamp, attn none) with `GX_VA_NRM` absent (`0x1008` bits 3:2 = 0). RT substitutes `(0,0,1)` → 89; a zero normal would give 0; hardware may instead reuse the last normal sent, so draw a vertex with normal `(0,0,−1)` immediately before (→ 0 if reused). |
| F.27 | Zero-length normal after the matrix | F.17a with vertex normal `(0,0,0)`. |
| F.28 | Channel 1, and G/B components | Repeat F.3, F.6 and F.20 on channel 1 and on the green and blue bytes; the silicon test covers only channel 0 red. |

---

# Part G — Where this document differs from the companions and the runtime

* **`gc-lighting-texgen.md` §A.4.2** says the spot cosine is "`dot(normalize(lightPos − vertexPos),
  lightDir_stored)`" — consistent with §B.4.2 here. No disagreement on the formula; this document
  adds the Dolphin-only `u ≥ 0` clamp as an open item.
* **§A.2.1 (the `GX_SetChanCtrl()` `ALPHA1` clobber)** is not recorded in either companion.
* **§A.6 / F.18:** `gc-xf-registers.md` §A.2 gives `0x0400 + 3·index`; the runtime implements
  `0x0400 + 9·(index/3)` (`gc_gx_draw.c:499-500`). Identical for libogc's indices, different for
  others.
* **Runtime vs Dolphin, predicted from reading `gc_gx_draw.c`:** the runtime does not clamp the spot
  cosine (F.8), has no specular back-face gate (F.13), does not clamp `N·H` (F.14), does not
  normalise `k` in the specular-with-diffuse state (F.15), returns 0 for a zero denominator (F.11)
  and rounds the float sum half-up once (F.20). Each of these is a place where the runtime and
  Dolphin will disagree and hardware must decide; none is established as a runtime bug.
* **Both lit channels share `mat`/`amb` vertex bytes and `has_color`** (`gc_gx_draw.c:280-291`), so
  F.23c tests the runtime's colour routing, not its lighting.

---

# What could not be determined

In descending order of how much a wrong choice would show in games:

1. **The numeric format of the lighting datapath** (§C.1) and therefore how fractional light
   contributions become the integer that §C.2's multiply consumes (§C.3). Three implementations make
   three choices. Settled by F.20; finer fractions if none matches.
2. **Whether the transformed normal is renormalised** (Part D). Every non-uniformly scaled model
   depends on it. Settled by F.17a/b.
3. **Whether the spot cosine is clamped at zero before the angular polynomial** (§B.4.2). Affects any
   spotlight with `a0 > 0` or `a1 < 0` — including `GX_SP_RING2` — on vertices behind the light.
   Settled by F.8.
4. **The three specular details** — back-face gate on `N·L`, clamp of `N·H`, and `k` normalisation
   when the diffuse field is non-zero (§B.4.3). The first two are visible on every specular object's
   dark side; the third needs a direct register write. Settled by F.13, F.14, F.15.
5. **Where the illumination is clamped to `[0,255]` and in what order lights are added** (§C.4). Only
   visible with signed diffuse or negative attenuation. Settled by F.21.
6. **Whether light direction words are normalised by the hardware** (§B.5). Dolphin-HW does and flags
   it as untested; Dolphin-SW and the runtime do not. Settled by F.16.
7. **Zero-denominator and zero-distance behaviour** (§B.5). Reachable from libogc via
   `GX_InitLightShininess(lobj, 0)` and stale coefficients. Settled by F.11 and F.24.
8. **Which normal-matrix address a position index that is not a multiple of 3 selects**, and whether
   indices above 31 are masked (§A.6). Settled by F.18 (and a variant with index 33).
9. **Register values libogc never writes:** attenuation field 0, diffuse field 3 (§A.2), and bits
   31:15 of the channel control word. F.25a/b.
10. **What a lit vertex uses when the vertex format has no normal**, and whether `0x1008` bits 3:2 gate
    anything (§A.4). F.26.
11. **Vertex colour routing when only `GX_VA_CLR1` is present** (§A.4) — documented in `gx.h`,
    unobserved. F.23c.
12. **That the light position and direction are not transformed by any matrix** (§A.5) — strongly
    implied by `gx.h`, directly checked only by F.19 and F.18b.
13. **That Flipper's final rounding equals the Wii's** (§C.2, Provenance item 5). The silicon test is a
    Wii program. Very likely; confirmed only by running `gxtest_lighting` on a GameCube.
14. **That hardware normalises `L` and uses the Euclidean distance for `v`** (§B.3, §B.4.2). Every
    implementation and the header's wording assume both; nothing measures them. F.5–F.7 would expose a
    violation as a mismatch against every reference value at once.
