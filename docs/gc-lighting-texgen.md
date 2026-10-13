# GameCube Lighting and Texture Coordinate Generation

## Provenance

This document covers the two parts of the Flipper transform unit (XF) that *compute* rather than
merely transform: the **per-channel lighting equation** and **texture coordinate generation**.

It is a companion to [`gc-xf-registers.md`](gc-xf-registers.md), which establishes the XF address
space, the light memory block layout, the channel control word and the texgen info registers.
**That document owns the bit tables; this one owns the arithmetic.** Register layouts are cited
here, not repeated.

Sources, in priority order:

1. **`gx.c`** — devkitPro/libogc's GX implementation (zlib licence), the same 5199-line copy used
   for the sibling document (`scratchpad/gx.c`). Authoritative for what the API's light-setup
   helpers actually write into a light block and what `GX_SetChanCtrl()` / `GX_SetTexCoordGen2()`
   pack into the registers. Every non-obvious claim quotes the source expression with its line
   number.
2. **`gx.h`** — `D:\devkitPro\libogc\include\ogc\gx.h` (same licence). Supplies the numeric enum
   values (`GX_DF_*`, `GX_AF_*`, `GX_TG_*`, `GX_SRC_*`, `GX_SP_*`, `GX_DA_*`, the light IDs and the
   matrix-slot constants) and the API documentation comments, which state the attenuation equation
   in closed form. **Reading `gx.h` resolves six of the fourteen items the sibling document listed
   as unverified**; §D notes which.
3. **`roms/gc/hwtests/gxtest/lighting.cpp`** — Dolphin's hardware test (GPL-2.0). Read **for
   behaviour only**; no code, comment text or wording is reproduced. It supplies the one fact in
   this document that comes from real silicon rather than from a header: the exact integer rounding
   of the final material-times-illumination step (§A.2.3).

Anything that could not be established from these three is marked **Unverified:**.

Helper macros are the standard libogc ones described in `gc-xf-registers.md`: `_SHIFTL(v,s,w)`
places the low `w` bits of `v` at bit `s`; `_SHIFTR(v,s,w)` extracts `w` bits from bit `s`.

---

# Part A — Lighting

## A.1 The light block, field by field

A light is 16 consecutive 32-bit words in XF memory at `0x0600 + 16i` for light *i*, *i* = 0..7.
The address arithmetic, the 16-word burst and the word ordering are established in
`gc-xf-registers.md` §A.4. What follows is what each word *means*.

`GX_LoadLightObj()` (`gx.c:4587-4641`) is the only place the block is assembled, and it writes the
sixteen words unconditionally in this order:

| Word | Field | Type | Written by |
|---|---|---|---|
| 0–2 | Reserved, always zero | — | `gx.c:4625-4627` |
| 3 | Light colour, RGBA8 | u32 | `GX_InitLightColor()` — `gx.c:4584` |
| 4 | `a0` — cosine attenuation, constant term | f32 | `GX_InitLightAttn()` / `GX_InitLightAttnA()` / `GX_InitLightSpot()` |
| 5 | `a1` — cosine attenuation, linear term | f32 | same |
| 6 | `a2` — cosine attenuation, quadratic term | f32 | same |
| 7 | `k0` — distance attenuation, constant term | f32 | `GX_InitLightAttn()` / `GX_InitLightAttnK()` / `GX_InitLightDistAttn()` |
| 8 | `k1` — distance attenuation, linear term | f32 | same |
| 9 | `k2` — distance attenuation, quadratic term | f32 | same |
| 10–12 | Position X, Y, Z | f32 | `GX_InitLightPos()` — `gx.c:4576-4578` |
| 13–15 | Direction X, Y, Z | f32 | `GX_InitLightDir()` — `gx.c:4690-4692` |

### A.1.1 Colour (word 3)

`GX_InitLightColor()` (`gx.c:4581-4585`):

```c
lit->col = ((_SHIFTL(col.r,24,8))|(_SHIFTL(col.g,16,8))|(_SHIFTL(col.b,8,8))|(col.a&0xff));
```

**RGBA8, red in the most significant byte** — the same byte order as the ambient and material
colour registers (`gc-xf-registers.md` §B.7). Four independent 8-bit unsigned values, `0..255`.

The light's alpha byte participates in the **alpha** channel's lighting equation, which is computed
by a separate channel control register (`0x1010`/`0x1011`) from the same light block. One light
block therefore feeds up to four independent channel computations (colour 0, colour 1, alpha 0,
alpha 1), each with its own enable, light mask, diffuse function and attenuation function.

### A.1.2 The three cosine-attenuation coefficients (words 4–6)

`a0`, `a1`, `a2` are the coefficients of a quadratic **in the cosine of an angle**, not in an
angle. Physically they shape the spotlight cone: how brightness falls off as the surface point
moves away from the cone axis. They are also reused, unchanged, as the *specular* falloff shape
when the channel's attenuation function is `GX_AF_SPEC` (§A.4.3).

They are **not** the cone angle, the exponent, or any other directly physical quantity. §A.5 shows
the conversions.

### A.1.3 The three distance-attenuation coefficients (words 7–9)

`k0`, `k1`, `k2` are the coefficients of a quadratic **in the distance** from the light position to
the vertex, forming the denominator of the attenuation ratio. `k0 = 1, k1 = 0, k2 = 0` means "no
distance attenuation" — note that this is a *unit* denominator, not a zero one; see §A.5.2.

### A.1.4 Position (words 10–12)

The light's position in the same space as the transformed vertex position. `GX_InitLightPos()`
(`gx.c:4572-4579`) stores the three floats verbatim — no negation, no normalisation.

**Unverified:** which space, exactly. `gx.h:4706-4707` requires the light *direction* to be
"transformed to view space", consistent with a vertex normal transformed by a normal matrix, which
strongly implies the position is likewise expected in view (eye) space and that the XF does **not**
transform the light block by any matrix. Neither source states this for the position outright.

### A.1.5 Direction (words 13–15) — **negated on the way in**

`GX_InitLightDir()` (`gx.c:4686-4693`):

```c
lit->nx = -(nx);
lit->ny = -(ny);
lit->nz = -(nz);
```

The API takes the direction the light *points* and stores its negation. The words in XF memory
therefore point **from the illuminated surface towards the light**. An emulator reading the light
block must use them as-is; an emulator comparing against GX source code must remember the flip.

`GX_InitLightDir()` does not normalise. It is the caller's responsibility to supply a unit vector,
and `GX_InitSpecularDir()` (which does normalise, `gx.c:4789-4794`) is the one place libogc does it.

### A.1.6 The specular aliasing of position and direction

There is no "specular light" bit in the light block. A specular light is an ordinary light block
whose position and direction words have been loaded with something else entirely.
`GX_InitSpecularDir()` (`gx.c:4779-4806`):

```c
hx  = -nx;
hy  = -ny;
hz  = (-nz + 1.0f);
mag = ((hx * hx) + (hy * hy) + (hz * hz));
if(mag!=0.0f) mag = 1.0f / sqrtf(mag);
hx *= mag; hy *= mag; hz *= mag;

px  = (nx * LARGE_NUMBER);
py  = (ny * LARGE_NUMBER);
pz  = (nz * LARGE_NUMBER);
```

* The **position** words receive the light direction scaled by `LARGE_NUMBER`, which is
  `-1048576.0f` on GameCube and `-1.0e+18f` on Wii (`gx.c:22-26`). Scaling by a large *negative*
  number both pushes the light effectively to infinity — making it parallel — and flips the sign so
  that the vertex-to-light vector comes out pointing along `+n`.
* The **direction** words receive the **half-angle vector** `normalize(-n + (0,0,1))`, i.e. the
  half-vector between the light direction and the +Z eye direction. The hard-coded `(0,0,1)` is why
  a specular light only works in view space with the eye looking down −Z.
* `GX_InitSpecularDirHA()` (`gx.c:4762-4777`) is the same thing with the half-angle supplied by the
  caller instead of derived.

`gx.h:4913-4916` is explicit that `GX_InitLightDir()` and `GX_InitLightPos()` must **not** be used
on a light object destined for specular use, because they overwrite these fields.

---

## A.2 The per-channel equation

A channel is one of four independent computations selected by the four lit-channel registers
`0x100E` (colour 0), `0x100F` (colour 1), `0x1010` (alpha 0), `0x1011` (alpha 1). The bit layout of
the word is `gc-xf-registers.md` §B.6. The fields that drive the equation are:

| Bit(s) | Field | Enum values (now confirmed from `gx.h`) |
|---|---|---|
| 0 | Material source | `GX_SRC_REG` = 0, `GX_SRC_VTX` = 1 (`gx.h:172-173`) |
| 1 | Lighting enable | |
| 5:2, 14:11 | Light mask, low nibble then high nibble | |
| 6 | Ambient source | `GX_SRC_REG` = 0, `GX_SRC_VTX` = 1 |
| 8:7 | Diffuse function | §A.3 |
| 10:9 | Attenuation function | §A.4 |

### A.2.1 Lighting disabled (bit 1 clear)

```
channel_out = material
```

The material colour is passed straight through, from the material register (`0x100C`/`0x100D`) or
from the vertex colour according to bit 0. No ambient term, no lights, no arithmetic at all.
`gx.h:1846-1848` states this for the API; there is nothing in the equation to round, so the value
is bit-exact.

### A.2.2 Lighting enabled — the illumination accumulator

With bit 1 set, the hardware builds an **illumination value** per component and then modulates the
material by it:

```
illum = ambient                                   (8-bit, from register 0x100A/0x100B or vertex)
for each light L in the channel's 8-bit mask:
    illum += light_contribution(L)
channel_out = material * widen8to9(clamp255(illum)) >> 8
```

where

```
light_contribution(L) = L.colour * diffuse_term(L) * attenuation(L)
```

The **ambient term is the starting value of the accumulator**, not a separate additive stage
applied afterwards, and it is not multiplied by the material separately. This is provable: see
§A.6.

The **material term is a multiplier applied once at the very end**, not per light. That is the
single most important structural fact about the equation, and it is what the hardware test in
§A.2.3 measures.

### A.2.3 The eight-bit widening and the truncation — exactly

`lighting.cpp` drives a channel with lighting enabled, ambient source and material source both set
to the registers, and an **empty light mask**, then sweeps all 256 × 256 combinations of an 8-bit
ambient value and an 8-bit material value and reads the rasterised red channel back out of the EFB
(`lighting.cpp:52-88`). The result matches, for every one of the 65536 cases:

```
out = ( mat * ( illum + (illum >> 7) ) ) >> 8
```

with `mat` and `illum` both unsigned 8-bit. Decomposed:

1. **Widen 8 bits to 9.** `illum + (illum >> 7)` maps `0 → 0`, `1..127 → 1..127`,
   `128..255 → 129..256`. It is the standard "replicate the top bit" widening: it makes `255`
   become `256`, so that a fully-lit channel is an exact unit multiplier. It is applied to the
   **illumination** value, never to the material.
2. **Multiply.** An 8-bit material times a 9-bit illumination gives a 17-bit product.
3. **Truncate.** `>> 8` with **no rounding term added** — not `+128`, not `+0x80`, nothing. The
   result is the floor of `mat * illum / 255`-ish, and it is biased low by up to one LSB relative
   to a correctly rounded computation.

Two consequences an implementer must not smooth over:

* `mat = 255, illum = 255` gives `(255 * 256) >> 8 = 255`. Exact white.
* `mat = 255, illum = 127` gives `(255 * 127) >> 8 = 126`, **not** 127. The widening is
  discontinuous at 128, and a floating-point `mat * illum / 255.0` implementation will disagree
  with hardware over large parts of the range.

**Unverified:** whether the same widen-and-truncate applies to the multiplication of a light's
colour by its diffuse and attenuation factors. The hardware test isolates only the final stage,
because it uses an empty light mask. The per-light multiply is plausibly done in a wider fixed-point
format and only collapsed to 8 bits at the accumulator; nothing here proves it either way.

### A.2.4 Accumulation order and saturation

**Unverified:** both.

What can be said:

* The light mask is an 8-bit selection reassembled from bits 5:2 and 14:11 (`gc-xf-registers.md`
  §B.6), with bit *n* of the reassembled mask meaning light *n*. The API enum is a bitmask, not an
  index — `GX_LIGHT0` = `0x001` through `GX_LIGHT7` = `0x080` (`gx.h:178-185`) — and
  `GX_SetChanCtrl()` passes it through with only the nibble split, so a channel may enable any
  subset of the eight lights. Ascending light index is the natural order and the only one for which
  the mask encoding makes sense, but neither source proves the hardware accumulates in that order.
* Whether accumulation **saturates at 255 per light** or accumulates wider and clamps once at the
  end is not determinable here. It matters: with three lights each contributing 100, a saturating
  accumulator gives 255 either way, but with contributions that can go *negative* (which
  `GX_DF_SIGNED` permits, §A.3) an intermediate clamp at zero would discard information that a
  late clamp would keep. The `>> 8` step in §A.2.3 requires the value entering it to be at most 255
  for the widening to behave, so a clamp to `[0, 255]` exists **somewhere** before the final
  multiply; its position relative to the per-light adds is the open question.

---

## A.3 Diffuse functions (bits 8:7)

Numeric values, now confirmed from `gx.h:193-195`:

| Value | Name | What it does to `N · L` |
|---|---|---|
| 0 | `GX_DF_NONE` | The diffuse factor is **1.0**. `N · L` is not computed and the vertex normal is irrelevant to this channel. |
| 1 | `GX_DF_SIGNED` | The diffuse factor is `N · L` **used as-is, including negative values**. This is the signed one. Surfaces facing away from the light *subtract* from the accumulator. |
| 2 | `GX_DF_CLAMP` | The diffuse factor is `max(N · L, 0)` — clamped to zero, the ordinary Lambert term. |
| 3 | — | **Unverified:** never produced by `GX_SetChanCtrl()`. |

`L` is the **normalised** vector from the vertex position to the light position; `N` is the
transformed vertex normal. Neither is normalised by the setup helpers, so the normalisation of `L`
is a hardware step. **Unverified:** whether the hardware renormalises `N` (a normal matrix with
non-uniform scale produces a non-unit normal); nothing in either source addresses it.

### A.3.1 Specular forces the diffuse function to zero

`GX_SetChanCtrl()` (`gx.c:2195`):

```c
u32 reg,difffn = (attn_fn==GX_AF_SPEC)?GX_DF_NONE:diff_fn;
```

With `GX_AF_SPEC` selected, the caller's diffuse function is **discarded** and `GX_DF_NONE` (0) is
written. The `N · L` slot in the datapath is occupied by the specular computation, which uses
`N · H` instead (§A.4.3). An emulator should still honour whatever bits 8:7 actually contain in the
register — a title that pokes the register directly can set a non-zero diffuse function alongside
specular, and the hardware behaviour in that case is **Unverified**.

---

## A.4 Attenuation functions (bits 10:9)

Numeric API values, confirmed from `gx.h:201-203`: `GX_AF_SPEC` = 0, `GX_AF_SPOT` = 1,
`GX_AF_NONE` = 2. libogc does not pass the enum through; it derives the two register bits
arithmetically (`gx.c:2196`):

```c
|(_SHIFTL(((GX_AF_NONE-attn_fn)>0),9,1))|(_SHIFTL((attn_fn>0),10,1))
```

Substituting `GX_AF_NONE` = 2 gives, with certainty:

| Register value (bits 10:9) | bit 10 | bit 9 | API input | Meaning |
|---|---|---|---|---|
| 0 | 0 | 0 | — | Not produced by `GX_SetChanCtrl()`. **Unverified:** presumed "no attenuation", same as 2. |
| 1 | 0 | 1 | `GX_AF_SPEC` (0) | Specular |
| 2 | 1 | 0 | `GX_AF_NONE` (2) | No attenuation — `atten` = 1 |
| 3 | 1 | 1 | `GX_AF_SPOT` (1) | Spot: cosine falloff over distance falloff |

Read the bits separately and the encoding is systematic: **bit 9 means "an attenuation ratio is
evaluated", bit 10 means "this is not a specular light"**. This confirms the sibling document's
table and removes its two enum caveats.

### A.4.1 The ratio of two quadratics

`gx.h:4794` states the equation in closed form. Transcribed into ordinary notation:

```
                clamp0( a2 * u^2  +  a1 * u  +  a0 )
    atten  =   -------------------------------------
                       k2 * v^2  +  k1 * v  +  k0
```

* `clamp0(x)` is `max(x, 0)` — **the numerator is clamped to zero, the denominator is not**. A
  negative denominator is therefore reachable and produces a negative `atten`.
* `u` and `v` are different quantities in the two modes; that is the whole difference between spot
  and specular. See §A.4.2 and §A.4.3.
* The whole ratio multiplies the **light colour**; it does not touch the diffuse term, which is a
  separate factor (§A.2.2).

**The header contains a typo, and it is a trap.** `gx.h:4794` prints the numerator's leading term
as `a2^2 * aattn^2`. `a2` is **not** squared. This is provable from `GX_InitLightSpot()` by algebra:
for `GX_SP_COS2` (`gx.c:4829-4833`) the coefficients are `a0 = 0`, `a1 = -cr/(1-cr)`,
`a2 = 1/(1-cr)`, which under the unsquared reading give `f(u) = u(u - cr)/(1 - cr)` — exactly zero
at the cutoff `u = cr` and exactly one at the axis `u = 1`, as a spot distribution must be. Under
the squared reading the axis value would be `1/(1-cr)^2 - cr/(1-cr)`, which is not 1 for any
`cr != 0`. Likewise `GX_SP_SHARP` writes `a2 = -1.0/d` (`gx.c:4838`); squaring a negative
coefficient would destroy the sign the curve depends on. **The correct numerator is
`a2*u^2 + a1*u + a0`.**

### A.4.2 The spot / cosine path (`GX_AF_SPOT`, register value 3)

Per `gx.h:4796-4799`:

* `u` = **the cosine of the angle between the light direction and the vector from the light
  position to the vertex**. In terms of the stored words (which are already negated, §A.1.5) this
  is `dot(normalize(lightPos - vertexPos), lightDir_stored)`.
* `v` = **the distance** `|lightPos - vertexPos|`, in the same units as the position words.

So the numerator quadratic is in a **cosine** (dimensionless, nominally in `[-1, 1]`) and the
denominator quadratic is in a **length** (unbounded, in world units). The two quadratics do not
share a variable. Getting this wrong — feeding the cosine into the `k` quadratic — is the most
common way a spot light comes out looking like a specular highlight.

**Unverified on hardware, and now done:** `u` is clamped to zero before the numerator is
evaluated, and a `u` that is not a number is zero (`attenuate` in `gc_gx_draw.c`, 2026-10-08).
Dolphin's software renderer and its shaders both take `max(0, cosine)` here. What forced the
choice is a game, not a measurement: Super Mario Galaxy 2 lights its flowers with lights whose
cone is off (`a0 = 1, a1 = a2 = 0`) and whose direction words it never writes. They hold what its
stack held, which is sometimes not a number, and IEEE's `0 * NaN` made the whole channel black.
A console draws those flowers lit, so whatever its arithmetic does with such words, it does not
do that. The upper bound is not clamped: a normalised direction keeps the cosine at or below one.

### A.4.3 The specular path (`GX_AF_SPEC`, register value 1)

Per `gx.h:4807-4808`, in specular mode **`u` and `v` are the same quantity**: both are
`dot(N, H)`, the dot product of the eye-space vertex normal with the half-angle vector stored in the
light block's direction words (§A.1.6). The denominator's quadratic therefore evaluates in the
cosine too, and the ratio becomes a rational approximation to `(N·H)^shininess` — `gx.h:5308-5312`
says so explicitly, and explains that the specular unit is literally the diffuse attenuation unit
reused, which is why the exponent has to be faked with a ratio of quadratics.

The convenience macro (`gx.h:5319-5322`) makes the shape concrete:

```c
#define GX_InitLightShininess(lobj, shininess) \
    (GX_InitLightAttn(lobj, 0.0F, 0.0F, 1.0F,  \
                    (shininess)/2.0F, 0.0F,   \
                    1.0F-(shininess)/2.0F ))
```

so `a = (0, 0, 1)` and `k = (s/2, 0, 1 - s/2)`, giving

```
    atten = c^2 / ( s/2 + (1 - s/2) * c^2 )      with c = N·H
```

which is 1 at `c = 1` for every `s` (the two `s/2` terms cancel), and falls off faster as `s` grows.
For `s > 2` the constant term `k0 = s/2 > 1` and `k2 = 1 - s/2 < 0`, so the denominator *decreases*
with `c` — the approximation sharpens by shrinking the denominator rather than by raising a power.

**Unverified:** whether the hardware gates the specular contribution on `N · L >= 0` (a back-face
test using the light's *position* words, which in specular mode hold the direction scaled by
`LARGE_NUMBER`). Nothing in `gx.c` or `gx.h` describes such a gate, but without one a specular
highlight would appear on surfaces facing away from the light.

### A.4.4 Zero denominator

**Unverified.** Neither source states what the XF does when `k2*v^2 + k1*v + k0` evaluates to zero.

What is certain is that **libogc never produces a coefficient set that can reach zero at `v = 0`**:
every branch of `GX_InitLightDistAttn()` sets `k0 = 1.0f` (`gx.c:4705, 4710, 4715, 4721`), and the
"off" case is `k = (1, 0, 0)`, a constant denominator of 1. The reachable danger cases are
(a) a title calling `GX_InitLightAttn()` / `GX_InitLightAttnK()` with `k0 = 0` and then lighting a
vertex at the light's exact position, and (b) the `GX_InitLightShininess()` set with `s = 0`, which
gives `k = (0, 0, 1)` and therefore a zero denominator whenever `N·H = 0` — the numerator is
`c^2 = 0` there too, so it is a genuine `0/0`.

An emulator has three defensible choices — propagate IEEE infinity/NaN, clamp, or special-case
`0/0` to zero — and no evidence here distinguishes them. Record the choice; do not present it as
hardware behaviour.

---

## A.5 Turning an angle or a distance into coefficients

This is the section an implementation most often gets wrong, because **the six coefficients are not
the physical quantities**. A cutoff angle is not `a0`; a reference distance is not `k1`. The
conversions below run on the CPU, in the game's own code, before the light block is ever written —
so an emulator that only decodes XF writes never sees them. They matter for two reasons: porting GX
code, and sanity-checking a decoder against a known light setup.

### A.5.1 `GX_InitLightSpot()` — angle to `a0`, `a1`, `a2`

`gx.c:4808-4863`. The function takes a **cutoff angle in degrees** and a distribution function, and
writes only `a0`, `a1`, `a2`.

```c
if(cut_off<0.0f ||	cut_off>90.0f) spotfn = GX_SP_OFF;

r = (cut_off*M_PI)/180.0f;
cr = cosf(r);
```

The only physical quantity that survives into the coefficients is **`cr`, the cosine of the cutoff
angle**. Everything else is curve shaping. Writing `d = (1 - cr)^2`:

| `spotfn` | Value | `a0` | `a1` | `a2` | Resulting `f(u)` |
|---|---|---|---|---|---|
| `GX_SP_OFF` | 0 | `1` | `0` | `0` | Constant 1 — no angular falloff |
| `GX_SP_FLAT` | 1 | `-1000*cr` | `1000` | `0` | `1000*(u - cr)` — a near-vertical ramp at the cone edge |
| `GX_SP_COS` | 2 | `-cr/(1-cr)` | `1/(1-cr)` | `0` | `(u - cr)/(1 - cr)` — linear in the cosine, 0 at the edge, 1 on axis |
| `GX_SP_COS2` | 3 | `0` | `-cr/(1-cr)` | `1/(1-cr)` | `u*(u - cr)/(1 - cr)` — same endpoints, quadratic interior |
| `GX_SP_SHARP` | 4 | `cr*(cr-2)` | `2/d` | `-1/d` | see the note below |
| `GX_SP_RING1` | 5 | `-4*cr/d` | `4*(1+cr)/d` | `-4/d` | `-4(u-1)(u-cr)/d` — peaks at 1 midway between edge and axis, 0 at both |
| `GX_SP_RING2` | 6 | `1 - 2*cr*cr/d` | `4*cr/d` | `-2/d` | `1 - 2(u - cr)^2/d` — peaks at 1 **on the cone edge**, falls off both ways |

Enum values from `gx.h:988-994`. Coefficient expressions from `gx.c:4819-4857`.

Three things to carry into an implementation:

**`GX_SP_FLAT` relies on downstream clamping.** `1000*(u - cr)` reaches 1 only
`0.001` of a cosine inside the cone edge, and then keeps climbing to values in the hundreds. It is
"flat" only because the accumulator saturates (§A.2.4). An emulator that does not clamp will render
a flat spot as a blinding disc.

**`GX_SP_SHARP` has a libogc bug.** The intended curve is the parabola that is zero at the cone edge
and peaks smoothly at 1 on axis: `f(u) = 1 - (1-u)^2/d`, whose coefficients are
`a0 = cr(cr-2)/d`, `a1 = 2/d`, `a2 = -1/d`. libogc writes `a1` and `a2` divided by `d` but writes
`a0` **undivided** (`gx.c:4836`), so `f(1) = 1/d + cr^2 - 2cr`, which equals 1 only when `cr = 0`,
i.e. only at a 90-degree cutoff. Every other cutoff angle gets an over-bright, wrongly-shaped cone.
This is a derivation, not a quotation: the algebra above is the evidence. An emulator is unaffected
(it sees whatever floats the title wrote), but anyone porting or re-deriving GX light setup must
know that libogc's `GX_SP_SHARP` does not match the documented curve, and that a title built against
the real SDK will have written different `a0` values.

**The domain guard admits zero.** `gx.c:4813` rejects `cut_off < 0` and `cut_off > 90`, but
`gx.h:4939-4940` documents the valid range as `0 < cut_off <= 90`. A cutoff of exactly `0` passes
the guard, gives `cr = 1`, hence `1 - cr = 0` and `d = 0`, and every branch except `GX_SP_OFF` and
`GX_SP_FLAT` then divides by zero and stores infinities or NaNs into the light block.

### A.5.2 `GX_InitLightDistAttn()` — distance to `k0`, `k1`, `k2`

`gx.c:4695-4730`. The function takes a **reference distance** and a **reference brightness** — "at
this distance the light should be this fraction of full brightness" — plus a curve selector, and
writes only `k0`, `k1`, `k2`. Abbreviating `D = ref_dist`, `b = ref_brite`:

| `dist_fn` | Value | `k0` | `k1` | `k2` |
|---|---|---|---|---|
| `GX_DA_OFF` | 0 | `1` | `0` | `0` |
| `GX_DA_GENTLE` | 1 | `1` | `(1-b)/(b*D)` | `0` |
| `GX_DA_MEDIUM` | 2 | `1` | `0.5*(1-b)/(b*D)` | `0.5*(1-b)/(b*D*D)` |
| `GX_DA_STEEP` | 3 | `1` | `0` | `(1-b)/(b*D*D)` |

Enum values from `gx.h:979-982`; expressions from `gx.c:4704-4724`.

`k0` is **always 1**. All three curves are constructed so that the denominator evaluates to exactly
`1/b` at `v = D`, hence `atten = b` at the reference distance — verifiable by substitution in each
row. They differ only in how the falloff is distributed between the linear and quadratic terms
before and after that point: `GENTLE` is purely `1/(1 + c*v)`, `STEEP` is purely `1/(1 + c*v^2)`,
`MEDIUM` splits the deficit evenly.

**The domain guard is looser than the documentation and lets division by zero through.**
`gx.c:4700-4701`:

```c
if(ref_dist<0.0f ||
    ref_brite<0.0f || ref_brite>=1.0f) dist_fn = GX_DA_OFF;
```

`gx.h:4765-4768` requires `ref_dist > 0` and `0 < ref_brite < 1`. The code accepts
`ref_dist == 0.0f` and `ref_brite == 0.0f`, both of which land in a divisor. The resulting
infinities go straight into the light block.

### A.5.3 The other setters

For completeness, the remaining writers of the six coefficients:

| Function | Line | Writes |
|---|---|---|
| `GX_InitLightAttn()` | `gx.c:4732-4742` | all six, verbatim, in the order `a0 a1 a2 k0 k1 k2` — the same order as the block |
| `GX_InitLightAttnA()` | `gx.c:4744-4751` | `a0 a1 a2` only |
| `GX_InitLightAttnK()` | `gx.c:4753-4760` | `k0 k1 k2` only |
| `GX_InitLightShininess()` | `gx.h:5319-5322` | a macro over `GX_InitLightAttn()`; see §A.4.3 |

None of them validates anything.

---

## A.6 Lighting enabled, empty light mask

**The channel outputs the material modulated by the ambient value**, with the ambient value passing
through the same widen-and-truncate of §A.2.3:

```
channel_out = ( material * ( ambient + (ambient >> 7) ) ) >> 8
```

This is not inference. It is precisely the configuration `lighting.cpp` sets up — lighting enable
set, both sources set to the registers, and the light mask left at zero because the whole channel
control word is zeroed before the enable bit is set (`lighting.cpp:25-33`) — and the test asserts
the formula above holds for all 65536 ambient/material pairs (`lighting.cpp:52-88`).

So an unlit-but-enabled channel is **not** black, and it is **not** the material colour. Three
common mistakes it rules out:

* Treating an empty mask as "lighting disabled" and passing the material through unmodulated. Wrong
  unless the ambient is 255.
* Initialising the illumination accumulator to zero and adding the ambient only when at least one
  light is enabled. Wrong; the ambient *is* the accumulator's initial value.
* Computing `material * ambient / 255` in floating point. Wrong for most inputs; see §A.2.3.

The same reasoning extends to the disabled case: with bit 1 clear the material is passed through
with no multiply at all (§A.2.1), so `enable = 0` and `enable = 1, mask = 0, ambient = 255` are
equivalent, and every other ambient value is not.

---

# Part B — Texture coordinate generation

The XF generates up to eight texture coordinates per vertex. The count comes from register `0x103F`
(a copy of BP `genMode` bits 3:0), and each generator is configured by a pair of registers,
`0x1040 + n` (what to generate) and `0x1050 + n` (what to do to it afterwards). Bit layouts:
`gc-xf-registers.md` §B.12, §B.13 and §B.14.

libogc always writes the pair together and always writes the count first
(`__GX_SetTexCoordGen()`, `gx.c:973-992`):

```c
if(__gx->dirtyState&0x02000000) GX_LOAD_XF_REG(0x103f,(__gx->genMode&0xf));
...
    GX_LOAD_XF_REG(texcoord,__gx->texCoordGen[i]);
    GX_LOAD_XF_REG((texcoord+0x10),__gx->texCoordGen2[i]);
```

`GX_SetNumTexGens()` (`gx.c:2937-2941`) sets only the BP `genMode` nibble and the dirty bit; the XF
copy is emitted lazily at the next draw. The two must agree — BP drives rasterisation interpolation,
XF drives generation.

## B.1 Texgen sources

The API's source enum (`gx.h:383-403`) is *not* what goes into the register. `GX_SetTexCoordGen2()`
maps it through a switch (`gx.c:2667-2717`) onto a 5-bit **source row** number that occupies bits
11:7 of `0x1040 + n`. Both columns matter, and they are not in the same order:

| API constant | Value (`gx.h`) | Source row (register bits 11:7) | `stq` (register bit 2) | Vertex attribute read |
|---|---|---|---|---|
| `GX_TG_POS` | 0 | **0** | 1 | Position (`GX_VA_POS`), all three components |
| `GX_TG_NRM` | 1 | **1** | 1 | Normal (`GX_VA_NRM`), all three components |
| `GX_TG_BINRM` | 2 | **3** | 1 | Binormal (`GX_VA_NBT`, second vector) |
| `GX_TG_TANGENT` | 3 | **4** | 1 | Tangent (`GX_VA_NBT`, third vector) |
| `GX_TG_TEX0`..`GX_TG_TEX7` | 4..11 | **5..12** | 0 | Texture coordinate 0..7 (`GX_VA_TEX0`..`7`), two components |
| `GX_TG_COLOR0` | 19 | **2** | 0 | Colour — see §B.2.4 |
| `GX_TG_COLOR1` | 20 | **2** | 0 | Colour — see §B.2.4 |
| anything else | — | **5** (default, `gx.c:2714-2716`) | 0 | Texture coordinate 0 |

Note the discontinuity: the API puts binormal and tangent at 2 and 3, but the hardware rows are 3
and 4, with row 2 reserved for colour. The enum order and the row order differ; a table lookup is
required, not an offset.

**The binormal and tangent rows only exist when the vertex declares them.** The XF vertex specs
register `0x1008` bits 3:2 must be 2, meaning "normal + binormal + tangent" (`gc-xf-registers.md`
§B.4). With value 1 only row 1 is populated and rows 3 and 4 read whatever the hardware supplies —
**Unverified:** what that is.

**The `stq` bit (bit 2) is the input width, and it is derived, not chosen.** It is set to 1 for
exactly the four three-component sources — position, normal, binormal, tangent (`gx.c:2670, 2674,
2678, 2682`) — and left 0 for texture coordinates and colours. It controls how the source row is
widened into the 4-vector the matrix multiplies:

| bit 2 | Input vector |
|---|---|
| 0 | `(A, B, 1, 1)` |
| 1 | `(A, B, C, 1)` |

so a two-component texture coordinate `(s, t)` enters as `(s, t, 1, 1)` and a position `(x, y, z)`
enters as `(x, y, z, 1)`. The fourth component is always 1, which is what makes column 3 of the
texture matrix a translation.

**The position and normal rows deliver the untransformed (model-space) attribute**, as the vertex
carried it: not the position after its position matrix, and not the normal after its normal matrix
and normalisation. Neither SDK source states it, and this was open until 2026-10-09; the runtime
had it the other way. What settled it:

* A retail game. Super Mario Galaxy 2's Luma is a skinned model with a reflection map: every vertex
  carries a texture matrix index beside its position matrix index, and the generator's source is the
  normal. Each bone's texture matrix turns that bone's raw normals into view space. Fed the
  transformed normal, each bone turned its normals twice, by a different amount, and the reflection
  broke along the lines between bones whenever the model was posed - a cracked shell, smooth only
  where it faced the camera at rest. With the raw normal it is continuous in every pose.
* A second implementation: Dolphin's software and hardware renderers both read the raw attribute
  (and put 1 in the third place for input form 0 whatever the row, which the runtime now does too).
* The convention itself: a texture matrix that contains the modelview is only right for an
  untransformed input.

Still not a hardware measurement. `gc_gx`'s "a generator's source" holds the behaviour.

## B.2 Texgen types

The API type enum (`gx.h:365-376`) is again not what goes into the register. The register's type
field is bits 6:4 and takes only four values; the API's eleven constants collapse onto them, with
the emboss light index and the colour channel moved into other fields
(`GX_SetTexCoordGen2()`, `gx.c:2719-2742`).

| API constant | Value | Register type (6:4) | Register projection (bit 1) | Other fields |
|---|---|---|---|---|
| `GX_TG_MTX3x4` | **0** | 0 | **1** | — |
| `GX_TG_MTX2x4` | **1** | 0 | **0** | — |
| `GX_TG_BUMP0`..`BUMP7` | 2..9 | 1 | 0 | light index `type - 2` into bits 17:15; source texcoord into 14:12 |
| `GX_TG_SRTG` + `GX_TG_COLOR0` | 10 | 2 | 0 | source row forced to 2 |
| `GX_TG_SRTG` + `GX_TG_COLOR1` | 10 | 3 | 0 | source row forced to 2 |

**The 2x4/3x4 enum values are inverted relative to the register bit.** `GX_TG_MTX3x4` is enum 0 but
sets projection bit 1; `GX_TG_MTX2x4` is enum 1 but clears it (`gx.c:2723`:
`if(tgen_typ==GX_TG_MTX3x4) texcoords = 0x02;`). Anyone transcribing the enum straight into the
register gets every matrix texgen backwards.

### B.2.1 `GX_TG_MTX2x4` — affine, two outputs

```
    (s, t) = M2x4 * input4
```

`M2x4` is two rows of four floats read from position/texture matrix memory at `0x0000 + 4*index`
(`gc-xf-registers.md` §A.1); the index is the 6-bit field for this texgen in matrix index register
`0x1018` or `0x1019` (§B.9). **No division.** Two multiply-accumulate rows of four terms each.

### B.2.2 `GX_TG_MTX3x4` — projective, divided

```
    (s, t, q) = M3x4 * input4
    output    = (s/q, t/q)
```

`gx.h:365-366` states the divide explicitly. This is the only texgen type that produces a
projective coordinate. The third matrix row exists solely to produce the divisor.

**Unverified:** the divide's position relative to the post-transform stage (§B.3), and the behaviour
when `q = 0`.

### B.2.3 `GX_TG_BUMP0`..`GX_TG_BUMP7` — emboss

What is **certain** from `gx.c:2727-2736`:

```c
tgen_src -= GX_TG_TEXCOORD0;
tgen_typ -= GX_TG_BUMP0;

texcoords = 0x10;
texcoords |= (_SHIFTL(stq,2,1));
texcoords |= (_SHIFTL(vtxrow,7,5));
texcoords |= (_SHIFTL(tgen_src,12,3));
texcoords |= (_SHIFTL(tgen_typ,15,3));
```

* The type field is 1.
* Bits 17:15 hold a **zero-based light index**, `tgen_typ - GX_TG_BUMP0` = `type - 2`, so
  `GX_TG_BUMP0` selects light 0 through `GX_TG_BUMP7` selecting light 7.
* Bits 14:12 hold a **zero-based texture coordinate index**, `tgen_src - GX_TG_TEXCOORD0` =
  `source - 12`, naming which *already-generated* texture coordinate supplies the base `(s, t)` that
  the emboss offset is added to. This generator therefore depends on another generator's output, and
  the source must be a lower-numbered texgen.
* **The source row field (bits 11:7) is always 5 and `stq` is always 0 for emboss, as a side effect
  of libogc's switch.** The emboss source constants `GX_TG_TEXCOORD0`..`6` are 12..18 and fall
  through every case of the switch at `gx.c:2667-2717` (whose highest case is `GX_TG_TEX7` = 11),
  landing on `default: vtxrow = 5;`. So for every emboss texgen libogc writes source row 5 and
  input-width 0 regardless of what the caller passed. Since the emboss path reads the binormal and
  tangent rows implicitly and takes its base coordinate from bits 14:12, this is harmless — but an
  emulator must not read bits 11:7 as meaningful when the type field is 1.
* **`GX_TG_TEXCOORD7` does not exist in `gx.h`.** The constants run `GX_TG_TEXCOORD0` = 12 through
  `GX_TG_TEXCOORD6` = 18, and then `GX_TG_COLOR0` = 19 (`gx.h:395-403`). The register field is 3
  bits and can express 7; the API cannot reach it except by accidentally passing `GX_TG_COLOR0`,
  which biases to exactly 7. Treat a value of 7 in bits 14:12 as "texture coordinate 7" in a
  decoder, but expect never to see it from libogc-built code.

**Unverified: the emboss arithmetic itself.** `gx.h:368-375` says only "use light N in the bump map
calculation". Neither source states how the binormal, the tangent, the light direction and the base
coordinate combine into the offset, nor what scale the offset carries, nor whether the light vector
is normalised first. The standard description — offset `s` by the light vector projected onto the
tangent and `t` by its projection onto the binormal — is consistent with the fields present, but it
is not evidenced here and the *magnitude* is certainly not.

### B.2.4 `GX_TG_SRTG` — colour to texture coordinate

Type values 2 and 3 (`gx.c:2737-2741`):

```c
} else if(tgen_typ==GX_TG_SRTG) {
    if(tgen_src==GX_TG_COLOR0) texcoords = 0x20;
    else if(tgen_src==GX_TG_COLOR1) texcoords = 0x30;
    texcoords |= (_SHIFTL(stq,2,1));
    texcoords |= (_SHIFTL(2,7,5));
}
```

* The colour **channel** is encoded in the *type* field (2 = channel 0, 3 = channel 1), not in the
  source row. The source row is forced to the literal 2 for both.
* The input is **the output of the lighting channel**, not the raw vertex colour. `gx.h:376-377` is
  explicit: coordinates are generated "from vertex lighting results; one of the color channel
  results is converted into texture coordinates". So `GX_TG_SRTG` consumes everything Part A
  computes — a channel with lighting disabled feeds its material colour in, a lit channel feeds the
  §A.2.3 result in.
* The conversion is red → S, green → T. This is the conventional mapping and is consistent with
  there being exactly two coordinates and four colour components, but **Unverified:** neither source
  states which components are used, nor the numeric scaling applied to the 8-bit values on their way
  into the coordinate.
* No matrix is involved and no matrix index is consulted. `GX_SetTexCoordGen2()` still writes the
  caller's `mtxsrc` into the matrix index register (`gx.c:2748-2773`), so a decoder will see a stale
  or meaningless index for this texgen; ignore it.

**Edge case:** if `GX_TG_SRTG` is passed a source that is neither `GX_TG_COLOR0` nor
`GX_TG_COLOR1`, neither branch assigns and `texcoords` stays at its initial `0` (`gx.c:2719`). The
register then reads as type 0, projection 0, source row 0 — a 2x4 matrix texgen on the position
row. libogc emits this silently.

## B.3 The post-transform (dual) matrix stage

Configured by `0x1050 + n`, built at `gx.c:2744-2746`:

```c
postmtx -= GX_DTTMTX0;
__gx->texCoordGen[txc] = texcoords;
__gx->texCoordGen2[txc] = ((_SHIFTL(normalize,8,1))|(postmtx&0x3f));
```

**When it applies.** Three gates, all of which must hold:

1. The global dual-texture-transform enable, `0x1012` bit 0, must be set. libogc sets it once in
   `__GX_InitRevBits()` and never clears it (`gc-xf-registers.md` §B.8), so for libogc-built titles
   it is always on.
2. `gx.h:2732-2734` states the post-transform options "only apply for 'ordinary' texgens, where the
   texgen type is `GX_TG_MTX2x4` or `GX_TG_MTX3x4`. They do not work for light-based texgens or
   emboss texgens." So register type field 0 only.
3. `gx.h:2745-2746` records a hardware fast path: when a vertex contains **only position and one
   texture coordinate** and the type is `GX_TG_MTX2x4`, **normalisation is skipped even when the bit
   is set**. The post-transform matrix itself is not described as skipped — only the normalisation.
   This is a data-dependent behaviour that no register write reveals; an emulator must detect it
   from the vertex descriptor.

**Whether it normalises first — yes, when bit 8 is set.** `gx.h:2736-2738`: the coordinate is
normalised "after the multiplication by `mtxsrc` (the first-pass transformation)", and "after the
optional normalization step, the texcoord is then multiplied by the 3x4 matrix `postmtx`". So the
order is:

```
    first-pass matrix  ->  optional normalise to unit length  ->  post-transform 3x4 matrix
```

Normalisation is what makes reflection mapping work: the first-pass matrix produces a direction, the
normalise makes it a unit vector on the sphere, and the post-transform matrix maps that into
`[0, 1]` texture space.

**Unverified:** whether the `GX_TG_MTX3x4` perspective divide (§B.2.2) happens before the normalise,
between normalise and post-transform, or after the post-transform matrix. The two plausible readings
give different results for any non-identity post-transform matrix, and `gx.h`'s prose describes the
divide and the dual-transform stage in separate places without relating them. Also **Unverified:**
whether the normalise operates on two or three components, and whether `0x1012` bit 0 gates the
normalise as well as the matrix.

**How the matrix index is selected.** Bits 5:0 of `0x1050 + n` — a per-texgen field in the
post-transform register itself, **not** a field of the matrix index registers `0x1018`/`0x1019`
(which carry the *first-pass* texture matrix indices only, `gc-xf-registers.md` §B.9). The index is
a row number in the `0x0500` block and is **already biased**: `GX_SetTexCoordGen2()` subtracts
`GX_DTTMTX0` before packing, so what the register holds is zero-based against `0x0500` and an
emulator must not re-apply the bias. Address = `0x0500 + 4 * index`.

The enum values, now readable from `gx.h`, make the block's layout concrete:
`GX_DTTMTX0` = 64 (`gx.h:241`) and `GX_DTTIDENTITY` = 125 (`gx.h:261`), so the biased identity index
is `125 - 64 = 61` and the identity matrix occupies rows 61–63 — the last three rows of the 64-row
block, with twenty 3-row matrix slots at rows 0, 3, … 57 below it. Post-transform matrices are
**always three rows**; `gx.h:2742-2743` says they are stored in the same format as first-pass
matrices "except that all matrices have three rows".

`GX_SetTexCoordGen()` — the one-line wrapper at `gx.c:2653-2656` — supplies `GX_FALSE` and
`GX_DTTIDENTITY`, so any title that never calls the `2` variant leaves normalisation off and every
post-transform matrix pointing at the identity.

## B.4 How a generated coordinate reaches the texture unit

Four stages, in order, on three different units:

**1. XF generates.** Up to `0x103F` coordinates are produced per vertex by the generators above.
Output is floating point, in whatever units the matrix produced — for the common case of a texture
matrix that maps model space to `[0, 1]`, normalised texture space.

**2. The rasteriser interpolates.** The BP `genMode` texgen count (bits 3:0) must match the XF
count, or the rasteriser interpolates a different number of coordinates than the transform unit
produced.

**3. The SU scales by texture size.** The generated coordinate is *normalised* — it does not know
the texture's dimensions. The scaling happens in the setup/texture unit, from the BP **SU size
registers** `0x30 + 2n` (S) and `0x31 + 2n` (T), whose bit layout is `gc-bp-registers.md` §8.5.
Two properties of those registers govern the whole mechanism:

* **They are indexed by texture *coordinate*, not by texture map.** `__SetSURegs()`
  (`gx.c:846-866`) computes `reg = (texcoord&0x7)` and writes the *bound map's* width and height
  into the register belonging to the *coordinate*. There are eight of each, one per texgen.
* **They hold size minus one.** The S register takes `texMapSize & 0x3ff` and the T register
  `_SHIFTR(texMapSize,10,10)` (`gx.c:853-859`), both already width−1/height−1;
  `GX_SetTexCoordScaleManually()` writes `(ss-1)` and `(ts-1)` explicitly (`gx.c:3674-3675`).

The consequence, which `gx.h:3021-3022` spells out, is that **a texture coordinate carries the scale
of whichever map was last bound to it**, so one coordinate can only be broadcast to several maps if
those maps are the same size. `GX_SetTevOrder()` is what performs the binding and therefore what
sets the scale; `GX_TEXMAP_DISABLE` exists precisely so a coordinate can be scaled by a map without
looking it up.

libogc recomputes these registers at draw time for every TEV stage's coordinate, direct and
indirect, but **only for coordinates not under manual control** — `__GX_SetSUTexRegs()`
(`gx.c:868-922`) skips any texcoord whose bit is set in `__gx->texCoordManually`, and
`GX_SetTexCoordScaleManually()` (`gx.c:3666-3679`) is what sets that bit and writes the size fields
by hand. An emulator does not need the shadow state, but a *trace* of BP writes will show the size
registers being rewritten on nearly every draw for automatically-managed coordinates and never for
manual ones.

**4. The coordinate is bound to a map per TEV stage.** Which of the eight coordinates a TEV stage
uses, and which map it looks up, come from the BP RAS order registers, not from the XF
(`gc-tev.md` §1.4). The XF's job ends when it hands over eight numbers.

---

# C. Implementation checklist

Facts that are certain and that an implementation must get right:

1. **The final lighting combine is `(mat * (illum + (illum >> 7))) >> 8`** — integer, widened on the
   illumination side only, truncated with no rounding term (§A.2.3). Hardware-verified.
2. **Lighting enabled with an empty light mask outputs the material modulated by the ambient**, not
   black and not the bare material (§A.6). Hardware-verified.
3. **Light directions in the block are pre-negated** — they point from surface to light
   (`gx.c:4690-4692`).
4. **`a` coefficients are a quadratic in a cosine; `k` coefficients are a quadratic in a distance**
   — except in specular mode, where both are the same `N·H` (§A.4.2, §A.4.3). The numerator is
   clamped to zero; the denominator is not.
5. **`a2` is not squared**, despite `gx.h:4794` (§A.4.1).
6. **`GX_DF_SIGNED` = 1 is the signed diffuse function; `GX_DF_CLAMP` = 2 clamps to zero**
   (`gx.h:193-195`). Specular forces the written diffuse function to 0 (`gx.c:2195`).
7. **Attenuation register values are 1 = specular, 2 = none, 3 = spot** — bit 9 is "evaluate a
   ratio", bit 10 is "not specular" (§A.4).
8. **The light mask is split across bits 5:2 and 14:11** of the lit-channel word
   (`gc-xf-registers.md` §B.6).
9. **`GX_TG_MTX3x4` is enum 0 and sets projection bit 1; `GX_TG_MTX2x4` is enum 1 and clears it**
   (§B.2). The enum and the register bit are inverted.
10. **The texgen source row is a lookup, not an offset** — binormal and tangent are API 2/3 but rows
    3/4, with row 2 reserved for colour (§B.1).
11. **`GX_TG_SRTG` reads the lit channel output**, not the raw vertex colour, and encodes the channel
    in the type field (§B.2.4).
12. **The post-transform matrix index lives in `0x1050 + n` bits 5:0, is pre-biased against
    `0x0500`, and always addresses a three-row matrix** (§B.3).
13. **SU size registers are indexed by texture coordinate and hold size minus one** (§B.4).

---

# D. What could not be determined

## D.1 Resolved from `gx.h` (previously unverified in `gc-xf-registers.md`)

Reading `gx.h` closes six of the sibling document's fourteen open items. They should be considered
settled:

* **Diffuse function enum values** (its item 6): `GX_DF_NONE` = 0, `GX_DF_SIGNED` = 1,
  `GX_DF_CLAMP` = 2 (`gx.h:193-195`).
* **Attenuation function enum values** (its item 7): `GX_AF_SPEC` = 0, `GX_AF_SPOT` = 1,
  `GX_AF_NONE` = 2 (`gx.h:201-203`), which pins the register-value mapping in §A.4 exactly.
* **Matrix slot enum values** (its item 12): `GX_PNMTX0` = 0, `GX_TEXMTX0` = 30, `GX_IDENTITY` = 60,
  `GX_DTTMTX0` = 64, `GX_DTTIDENTITY` = 125 (`gx.h:210, 225, 235, 241, 261`). The row partition
  tabulated in `gc-xf-registers.md` §A.1 and the identity-at-row-61 layout of §A.3 are confirmed.
* **The light IDs are a bitmask, not indices**: `GX_LIGHT0` = `0x001` … `GX_LIGHT7` = `0x080`
  (`gx.h:178-185`), which is why `GX_SetChanCtrl()` can take them ORed together and why
  `GX_LoadLightObj()` needs a switch to turn one back into an index (`gx.c:4593-4621`).
* **The colour/material source enum**: `GX_SRC_REG` = 0, `GX_SRC_VTX` = 1 (`gx.h:172-173`),
  confirming the polarity of bits 0 and 6 of the lit-channel word.
* **The attenuation equation's shape**, stated in closed form at `gx.h:4794` (with the `a2^2` typo
  noted in §A.4.1).

## D.2 Still unverified

Lighting:

1. **The coordinate space of the light position and direction words**, and whether the XF applies
   any matrix to them (§A.1.4). Inferred to be view space from `gx.h:4706-4707`, which only
   constrains the direction.
2. **Whether the per-light multiply uses the same widen-and-truncate as the final combine** (§A.2.3).
   The hardware test isolates only the final stage.
3. **The order lights accumulate in** (§A.2.4). Ascending index is the only sensible reading of the
   mask encoding, but is not evidenced.
4. **Whether the accumulator saturates per light or once at the end** (§A.2.4). A clamp to
   `[0, 255]` must exist before the final multiply; its position is unknown, and it matters as soon
   as `GX_DF_SIGNED` makes a contribution negative.
5. **Whether the hardware renormalises the transformed vertex normal** before the `N·L` dot (§A.3).
6. **Whether `u` is clamped to `[0, 1]` before the cosine quadratic is evaluated** (§A.4.2).
   `GX_SP_RING2`'s positive constant term would leak light behind the source without such a clamp.
7. **Whether specular is gated on `N·L >= 0`** (§A.4.3).
8. **Zero-denominator behaviour** in the attenuation ratio (§A.4.4). Unreachable through
   `GX_InitLightDistAttn()` (which always writes `k0 = 1`), reachable through
   `GX_InitLightAttn()` and through `GX_InitLightShininess(lobj, 0)`.
9. **Diffuse function value 3** (bits 8:7), never produced by `GX_SetChanCtrl()`.
10. **Attenuation function value 0** (bits 10:9), never produced by `GX_SetChanCtrl()`; presumed
    equivalent to value 2.
11. **Hardware behaviour when a title sets a non-zero diffuse function alongside specular**, which
    `GX_SetChanCtrl()` cannot produce but a direct register write can (§A.3.1).
12. **The meaning of words 0–2 of a light block** — carried over from `gc-xf-registers.md` §A.4;
    libogc always writes zero and never reads them back.

Texgen:

13. ~~Whether the position and normal texgen source rows deliver transformed or untransformed
    attributes~~ (§B.1). Untransformed: settled 2026-10-09 by a retail game's skinned reflection map
    and a second implementation, not by a hardware test. The emboss type still reads the view-space
    position (it measures to a light) and the *raw* binormal and tangent, where that implementation
    transforms them; nothing here has shown which is right.
14. **What rows 3 and 4 read when the vertex specs register declares normals but not
    binormals/tangents** (§B.1).
15. **The emboss arithmetic** (§B.2.3) — how binormal, tangent, light vector and base coordinate
    combine, at what scale, and whether the light vector is normalised. Only the *field assignments*
    are proven.
16. **Which colour components `GX_TG_SRTG` maps to S and T, and the numeric scaling applied**
    (§B.2.4). Red → S, green → T is conventional but unevidenced here.
17. **Where the `GX_TG_MTX3x4` perspective divide sits relative to the normalise and the
    post-transform matrix** (§B.2.2, §B.3), and the behaviour at `q = 0`.
18. **Whether the normalise step operates on two or three components**, and whether `0x1012` bit 0
    gates the normalise as well as the post-transform matrix (§B.3) — the latter carried over from
    `gc-xf-registers.md` §B.8.
19. **Bits 0 and 3 of the texgen info register `0x1040 + n`** — carried over from
    `gc-xf-registers.md` §B.13; never written by libogc, and they sit between two confirmed fields,
    so they are real bits of something.

## D.3 Two libogc defects worth recording

Neither affects an emulator reading a command stream — both run on the CPU before anything is
written — but both will mislead anyone reading libogc as a specification, and both change what a
libogc-built title puts in a light block relative to a title built against Nintendo's SDK.

* **`GX_SP_SHARP` fails to divide `a0` by `(1-cos θ)²`** (`gx.c:4836`), so the sharp spot
  distribution is correct only at a 90-degree cutoff. Derivation in §A.5.1.
* **Both coefficient helpers admit inputs that divide by zero.** `GX_InitLightSpot()` accepts
  `cut_off == 0` (`gx.c:4813`) against a documented range of `0 < cut_off <= 90`, and
  `GX_InitLightDistAttn()` accepts `ref_dist == 0` and `ref_brite == 0` (`gx.c:4700-4701`) against a
  documented range of `ref_dist > 0`, `0 < ref_brite < 1`. Infinities and NaNs reach the light
  block. An emulator should expect non-finite floats in light memory and decide deliberately what to
  do with them rather than assuming the values are sane.
