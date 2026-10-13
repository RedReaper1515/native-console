# GameCube Flipper: Fog and Indirect Textures

## Provenance

This document covers the semantics of the two pixel-pipeline blocks that
[`gc-bp-registers.md`](gc-bp-registers.md) records the *layout* of but not the *meaning* of: fog
(§13 there) and indirect textures (§11 there). Bit tables are **not** repeated here — where a
field is named below, its register number and bit position are in the sibling document.

Sources, in the same priority order the sibling document uses:

1. devkitPro/libogc `gx.c` (zlib licence). Authoritative for anything a libogc entry point
   computes. Line numbers refer to the same 5199-line copy the sibling document used
   (`scratchpad/gx.c`).
2. devkitPro/libogc `include/ogc/gx.h` (zlib licence) for enumerator values and the doxygen
   prose. Cited as `gx.h:NNN`.

Everything that follows is either quoted from those two files, or is algebra performed on quoted
expressions and shown in full so it can be re-checked. Claims that are neither are marked
**Unverified:**. As in the sibling document, a missing formula is better than a wrong one.

Note on the fog doxygen: `gx.h` documents the *meaning* of `GX_SetFog`'s parameters and the
shape of the range-adjust correction, but it does **not** state the per-type fog curves. Those
are marked Unverified in §A.4 below.

---

# Part A — Fog

## A.1 What the hardware actually stores

The hardware does not hold `startz`, `endz`, `nearz`, `farz`. It holds three numbers — **A**, **B**
and **C** — plus a **B shift** and a 3-bit function select, and evaluates a fixed rational
expression in the interpolated screen-space depth. `GX_SetFog()` (`gx.c:4249-4319`) is the
change of variables from the four distances to those constants.

| Hardware quantity | Register | Encoding |
|---|---|---|
| A | `0xEE` | IEEE-754 `float`, truncated to sign + 8-bit exponent + top 11 mantissa bits |
| B magnitude | `0xEF` | 24-bit unsigned integer |
| B shift (exponent) | `0xF0` | 5-bit unsigned integer |
| C | `0xF1` bits 19:0 | IEEE-754 `float`, truncated the same way as A |
| Projection | `0xF1` bit 20 | 0 = perspective, 1 = orthographic |
| Function select | `0xF1` bits 23:21 | see §A.4 |
| Fog colour | `0xF2` | 8:8:8 B, G, R — **no alpha** |

The A and C registers are the *top 20 bits of the raw `float` word*:
`val = 0xee000000|(_SHIFTR(a_hex,12,20));` (`gx.c:4305`) and
`(_SHIFTR(c_hex,12,20))` (`gx.c:4314`), where `a_hex`/`c_hex` come from a union type-pun
(`gx.c:4253, 4299-4303`). So reconstruction is:

```
sign = bit 19, exponent = bits 18:11 (IEEE biased), mantissa = bits 10:0
value = (-1)^sign * 2^(exponent - 127) * (1 + mantissa / 2048)          for exponent != 0
value = 0                                                               for exponent == 0 (see note)
```

The bottom 12 mantissa bits are **discarded, not rounded** — a plain right shift. An emulator that
stores the incoming 20 bits and reconstructs a `float` from them reproduces the hardware exactly;
one that keeps the caller's original `float` will drift.

**Unverified:** whether the hardware treats exponent 0 as a denormal or as a hard zero. libogc
only ever produces exponent 0 for a genuine `A = 0.0f` / `C = 0.0f` (`gx.c:4261-4262, 4275-4277`),
so the distinction is untestable from this source.

### The perspective derivation

`gx.c:4271-4298`:

```c
} else { // Perspective Fog Type
    if((farz==nearz) || (endz==startz)) {
        A = 0.0f;  B = 0.5f;  C = 0.0f;               // degenerate case
    } else {
        A = (farz*nearz)/((farz-nearz)*(endz-startz));
        B = farz/(farz-nearz);
        C = startz/(endz-startz);
    }

    B_mant = B;
    b_expn = 1;
    while(B_mant>1.0f)                  { B_mant /= 2.0f; b_expn++; }
    while((B_mant>0.0f) && (B_mant<0.5f)) { B_mant *= 2.0f; b_expn--; }

    A_f   = A/(1<<(b_expn));
    b_m   = (u32)(B_mant * 8388638.0f);
}
```

Three things to take from that:

1. **B is stored as a normalised mantissa in [0.5, 1) with its own binary exponent**, and the
   relationship is `B = B_mant * 2^(b_expn - 1)` — note the **−1**, because `b_expn` starts at 1
   while `B_mant` still equals `B`. Every iteration of either loop preserves
   `B_mant * 2^(b_expn-1)`, so the invariant holds on exit.
2. **The mantissa scale is 8388638.0f, not 8388608.0f (= 2^23).** That is a 30-unit offset, a
   relative error of about 3.6 × 10⁻⁶. It is in the shipped library and therefore in every game's
   register stream; reproduce the constant literally rather than "correcting" it to 2^23.
3. **A is pre-divided by 2^b_expn** before being converted to a float. A is *not* the A of the
   algebra above; the register holds `A / 2^b_expn`. This is the single most common way to get
   GameCube fog wrong.

For perspective projection with `far > near > 0`, `B = far/(far-near) >= 1`, so `b_expn >= 1`
always and the second normalising loop never runs. Two edge cases:

* `B == 1.0f` exactly (`near` → 0): neither loop runs, so `B_mant` stays at 1.0, which is
  *outside* the [0.5, 1) invariant. `b_m` then becomes 8388638, still inside 24 bits, and
  `b_expn` is 1. The reconstruction `B = B_mant * 2^0 = 1` is still correct, so this is harmless.
* `(u32)(1<<b_expn)` is an `int` shift (`gx.c:4296`). For `b_expn >= 31` it is undefined
  behaviour in C, and the `b_expn & 0x1f` mask at `gx.c:4311` silently wraps. Reaching that needs
  `far/(far-near) >= 2^30`, i.e. an essentially degenerate frustum.

### The orthographic derivation

`gx.c:4258-4270`:

```c
if(proj) { // Orthographic Fog Type
    if((farz==nearz) || (endz==startz)) {
        A_f = 0.0f;  C = 0.0f;
    } else {
        A   = 1.0f/(endz-startz);
        A_f = (farz-nearz) * A;
        C   = (startz-nearz) * A;
    }
    b_expn = 0;
    b_m    = 0;
}
```

So for orthographic fog: `A = (far - near)/(end - start)`, `C = (start - near)/(end - start)`, and
**both B registers are zero** — B is unused on this path, which is the clue that the orthographic
fog coordinate involves no division (§A.3).

### Degenerate inputs

`farz == nearz` or `endz == startz` produces A = 0, C = 0 on both paths (and B = 0.5 on the
perspective path, `gx.c:4276`; neither normalising loop runs on 0.5, so `B_mant = 0.5` and `b_expn = 1`, reconstructing to B = 0.5). The resulting
fog coordinate is identically 0, i.e. no fog. An emulator gets this for free by implementing the
general formula; it does not need a special case.

`GX_Init()` installs `GX_SetFog(GX_FOG_NONE,0,1,0.1,1,(GXColor)BLACK)` (`gx.c:705`).

## A.2 What the registers mean — the reconstruction

Combining the encodings, and writing `zs` for the normalised screen-space depth in [0, 1]:

**Perspective (`0xF1` bit 20 == 0):**

```
fogcoord = A/(B - zs) - C
```

with `A = A_reg * 2^b_shift` and `B = (b_magnitude / 8388638) * 2^(b_shift - 1)`.

Substituting the hardware's own representation — `b_magnitude = B * 2^(24 - b_shift)` and
`zs24 = zs * 2^24` (the 24-bit depth value the rasteriser already produces) — the whole
expression collapses to a form that needs no float exponent work:

```
fogcoord = (A_reg * 2^24) / (b_magnitude - (zs24 >> b_shift))  -  C
```

because `b_magnitude - (zs24 >> b_shift) = (B - zs) * 2^(24 - b_shift)` and
`A = A_reg * 2^b_shift`, so the two powers of two multiply out to exactly 2^24. This is why the
B shift exists at all: it is a pre-alignment that lets the hardware subtract the raw depth from
the raw B magnitude in one 24-bit integer subtract, with a single reciprocal afterwards.

**Unverified:** the integer rounding of that subtraction and reciprocal on real silicon. The
*algebra* above is exact and follows from `gx.c` alone; the *bit-exact* fixed-point behaviour of
the divider does not.

**Orthographic (`0xF1` bit 20 == 1):**

```
fogcoord = A_reg * zs - C
```

No division. B is zero and unused, which is consistent with `gx.c:4269-4270` writing zeros.

### Why these are the right formulas

This is the part worth checking, so here it is in full. Both paths must produce the same thing:
the eye-space distance normalised so that `startz` maps to 0 and `endz` maps to 1.

*Perspective.* A projection matrix consistent with `nearz`/`farz` gives normalised screen depth

```
zs = (far * (ze - near)) / (ze * (far - near)) = B - (B * near)/ze      where B = far/(far-near)
```

so `B - zs = (far * near) / ((far - near) * ze)`. Then

```
A/(B - zs) = [ (far*near) / ((far-near)(end-start)) ] * [ (far-near) * ze / (far*near) ]
           = ze / (end - start)
```

and subtracting `C = start/(end-start)` gives

```
fogcoord = (ze - start) / (end - start)                                        ✓
```

*Orthographic.* Here `zs = (ze - near)/(far - near)`, so

```
A_reg * zs = [ (far-near)/(end-start) ] * [ (ze-near)/(far-near) ] = (ze - near)/(end - start)
```

and subtracting `C = (start-near)/(end-start)` gives

```
fogcoord = (ze - start) / (end - start)                                        ✓
```

Both of libogc's parameter sets are exactly the coefficients that make the hardware's fixed
expression compute normalised eye-space range. That agreement across two independently written
branches is strong evidence that the two hardware expressions above are the real ones.

## A.3 The fog coordinate, step by step

1. The rasteriser interpolates screen-space depth `zs` across the primitive. The perspective
   divide has already happened at the vertex stage; `zs` is the same post-projection depth that
   feeds the Z buffer.
2. If Z textures are enabled, the Z-texture add/replace has already been applied:
   *"The Z-texture calculation is done before the fog range calculation"* (`gx.h:2778`). So the
   depth fog sees is the Z-textured depth.
3. **Perspective only:** the fog unit performs a reciprocal, `A/(B - zs)`, which recovers a value
   proportional to eye-space Z. This is the *second* division in the pipeline and the only one
   inside the PE. Orthographic fog skips it and multiplies instead.
4. The horizontal range adjustment (§A.5) scales that recovered range.
5. `C` is subtracted, producing the normalised coordinate that is 0 at `startz` and 1 at `endz`.
6. The function select (§A.4) maps that coordinate to a fog factor `f`.
7. `f` selects between the TEV colour and the fog colour (§A.6).

Granularity: `gx.h:3271` states the density is computed *"as a function of range to a quad (2x2
pixels)"*, and `gx.h:3296` repeats it for the range adjustment — *"computed per quad (2x2 pixels),
not per-pixel"*. A per-pixel implementation is more accurate than the hardware, not less, but it
will not match a reference trace exactly.

`gx.h:3271-3272` also notes that the range is *"cosine corrected Z in the x-z plane (eye
coordinates), but is not corrected in the y direction"* — that correction is precisely what §A.5
does.

**Unverified:** whether `fogcoord` is clamped to [0, 1] before the function is applied, and at
what precision. Nothing in `gx.c` or `gx.h` says. The formula can produce negatives (closer than
`startz`) and values above 1 (beyond `endz`), so *some* clamp must exist; its placement is not
derivable here.

## A.4 The fog types

The API enumerators (`gx.h:885-903`) are:

| `GX_FOG_*` | Perspective value | Orthographic value | `0xF1` bit 20 (proj) | `0xF1` bits 23:21 (fsel) |
|---|---|---|---|---|
| `NONE` | 0 | — | 0 | 0 |
| `LIN` | `PERSP_LIN` = 2 | `ORTHO_LIN` = 10 | 0 / 1 | 2 |
| `EXP` | `PERSP_EXP` = 4 | `ORTHO_EXP` = 12 | 0 / 1 | 4 |
| `EXP2` | `PERSP_EXP2` = 5 | `ORTHO_EXP2` = 13 | 0 / 1 | 5 |
| `REVEXP` | `PERSP_REVEXP` = 6 | `ORTHO_REVEXP` = 14 | 0 / 1 | 6 |
| `REVEXP2` | `PERSP_REVEXP2` = 7 | `ORTHO_REVEXP2` = 15 | 0 / 1 | 7 |

The split into two register fields is mechanical. `gx.c:4255` extracts the projection bit,

```c
proj = _SHIFTR(type,3,1);
```

and `gx.c:4314` writes `(_SHIFTL(type,21,3))|(_SHIFTL(proj,20,1))`. `_SHIFTL(v,21,3)` keeps only
the **low three bits** of `type`. So:

* **`0xF1` bits 23:21 = `type & 7`** — the curve selector.
* **`0xF1` bit 20 = `(type >> 3) & 1`** — orthographic flag.

The orthographic enumerators are exactly the perspective ones with bit 3 set (10 = 2|8,
12 = 4|8, …), and both families therefore land on the same fsel value. **There is only one set of
curves**; the projection bit changes how the coordinate is produced (§A.2), not which curve is
applied to it. `GX_FOG_ORTHO_LIN` and `GX_FOG_PERSP_LIN` both write fsel = 2.

Encoding values **1 and 3 are never produced by libogc** and their meaning is unknown.
**Unverified:** fsel 1 and fsel 3.

### The curves

Writing `z` for the normalised coordinate from §A.3 and `f` for the resulting fog factor
(0 = unfogged, 1 = fully fog-coloured):

| fsel | Name | Formula |
|---|---|---|
| 0 | None | `f = 0` — fog disabled; this is what `GX_Init()` selects. |
| 2 | Linear | `f = z` |
| 4 | Exponential | **Unverified**, see below |
| 5 | Exponential squared | **Unverified**, see below |
| 6 | Reverse exponential | **Unverified**, see below |
| 7 | Reverse exponential squared | **Unverified**, see below |

fsel 0 and fsel 2 are safe: "none" must yield no fog, and the entire `GX_SetFog` derivation
(§A.2) exists to make `z` run linearly from 0 at `startz` to 1 at `endz`, which is only
meaningful if the linear mode passes it through unchanged.

**Unverified: the four exponential curves.** Neither `gx.c` nor `gx.h` states them —
`gx.h`'s fog group (`gx.h:882-904`) is a bare list of enumerators with no prose, and `gx.c`
passes the type through as an opaque 3-bit value. The shapes that are widely used in
reimplementations are

```
exp      f = 1 - 2^(-8z)
exp2     f = 1 - 2^(-8 z^2)
revexp   f = 2^(-8(1-z))
revexp2  f = 2^(-8 (1-z)^2)
```

and they are self-consistent (each reverse form is its forward form reflected through
`z -> 1-z` and `f -> 1-f`, and all four satisfy `f(0) ≈ 0`, `f(1) ≈ 1`). **Do not treat the
constant 8, the use of base 2 rather than base e, or the placement of the square as established
by this document.** They must be confirmed against hardware before being relied on. If only one
thing here gets a hardware test, make it this.

## A.5 Range adjustment

### The problem

Fog density should depend on the *distance from the eye to the pixel*, but the value the fog unit
recovers in §A.3 is eye-space **Z** — the distance along the view axis. For a pixel at the left or
right edge of a wide field of view, the true range is longer than Z by a factor of
`1/cos(theta)`, where `theta` is the horizontal angle off the view axis. Without correction, a
wall of uniform depth fogs uniformly, when it should fog more heavily at the edges of the screen.

`gx.h:3294-3296`: the adjustment *"is a factor that is multiplied by the eye-space Z used for fog
computation; it is based upon the X position of the pixels being rendered. The Y direction is not
compensated."* And `gx.h:3271-3272`: range is *"cosine corrected Z in the x-z plane (eye
coordinates), but is not corrected in the y direction"*. So this is a horizontal-only cosine
correction, and its cost is one small table.

### The table

`GXFogAdjTbl` is ten `u16`s (`gx.h:1318-1323`), documented as *"u4.8 format range parameter"* —
4 integer bits and 8 fractional, held in the 12-bit register fields at `0xE9`–`0xED`. A value of
256 means "multiply range by 1.0"; the maximum representable factor is just under 16.

(Note: §13.2 of the sibling document calls this "8.4 fixed-point". The digits are transposed —
`gx.c:4343` computes `val6 * 256.0f` where `val6 >= 1`, so the fraction is 8 bits wide. **u4.8**
is correct.)

`GX_InitFogAdjTable()` (`gx.c:4321-4346`) fills it:

```c
if(projmtx[3][3]==0.0f) {
    val0 = projmtx[2][3]/(projmtx[2][2] - 1.0f);
    val1 = val0/projmtx[0][0];
} else {
    val1 = 1.0f/projmtx[0][0];
    val0 = val1*1.7320499f;
}

val2 = val0*val0;
val4 = 2.0f/(f32)width;
for(i=0;i<10;i++) {
    val5 = (i+1)*32.0f;
    val5 *= val4;
    val5 *= val1;
    val5 *= val5;
    val5 /= val2;
    val6 = sqrtf(val5 + 1.0f);
    val7 = (u32)(val6*256.0f);
    table->r[i] = (val7&0x0fff);
}
```

Reading it term by term:

* `projmtx[3][3] == 0` is the test for a **perspective** matrix (an orthographic one has 1 there).
* `val0 = m23/(m22 - 1)` **recovers the near-plane distance**. This works for either GX depth
  convention: with the OpenGL-style `m22 = -(f+n)/(f-n)`, `m23 = -2fn/(f-n)` it evaluates to
  `-2fn / (-(f+n) - (f-n)) = n`; with GX's own `[-1,0]`-range `m22 = -n/(f-n)`,
  `m23 = -fn/(f-n)` it evaluates to `-fn / (-n - (f-n)) = n`. Both give `n`.
* `m00` for a perspective matrix is `2n/(r-l)`, i.e. `near / (half-width of the frustum at the
  near plane)`. So `val1 = val0/m00` is **the eye-space half-width of the frustum at the near
  plane**.
* For the orthographic branch there is no near plane to recover, so libogc substitutes
  `val0 = val1 * 1.7320499f`. That constant is √3 = tan 60°, i.e. it *assumes* a 60° half-angle.
  The orthographic branch is therefore a fabricated stand-in, not a derivation.
* `val2 = val0²` = `near²`.
* `val4 = 2/width`, and `val5` starts at `32*(i+1)` — a horizontal offset in **pixels**. So
  `val5 * val4` is that offset in normalised device coordinates (±1 across the viewport), and
  multiplying by `val1` converts it to **eye-space X at the near plane**. Call it `x_i`.
* `val5 *= val5; val5 /= val2` gives `(x_i / near)²`, then `val6 = sqrt(1 + (x_i/near)²)`.

`sqrt(1 + (x/z)²)` is exactly `range / z` for a ray through eye-space `(x, z)` — the
`1/cos(theta)` factor the problem statement calls for. So:

```
table[i] = round_toward_zero( 256 * sqrt(1 + (x_i / near)^2) ),   i = 0..9
```

where `x_i` is the eye-space X, at the near plane, of a point **32·(i+1) pixels** to the side of
the viewport centre. Entry 0 covers 32 pixels out; entry 9 covers 320 pixels out — the edge of a
640-pixel-wide viewport. `table[i]` is always ≥ 256 and increases monotonically.

### How a pixel picks an entry

`GX_SetFogRangeAdj()` (`gx.c:4348-4370`) writes the ten entries into `0xE9`–`0xED` **only when
`enable` is true**, but always writes `0xE8`:

```c
val = 0xe8000000|(_SHIFTL(enable,10,1))|((center + 342)&0x03ff);
```

The `center` is the viewport's X centre, biased by +342 (`0x156`) — the same bias the scissor
registers use (sibling document §3.1), so the fog centre and the scissor share a coordinate
space. `gx.h:3297` states the function *"is mirrored horizontally about the `center`"*, so the
selector is `|x - center|`, and `GX_InitFogAdjTable`'s 32-pixel spacing means entry *k*
corresponds to `|x - center| = 32(k+1)`.

**Unverified: the exact index and interpolation arithmetic.** The following are *not* determinable
from `gx.c` or `gx.h`:

* how `|x - center|` splits into an integer index and a fractional weight (a natural reading is
  index `= (|dx| >> 5) - 1` with the low 5 bits as the blend weight, but that is inference);
* what value is used for `|dx| < 32`, where the table has no entry below index 0 — the physically
  correct value at `dx = 0` is exactly 1.0 (256), so the hardware plausibly interpolates from an
  implicit 256, but nothing here proves it;
* what happens past `|dx| = 320`, i.e. whether entry 9 is clamped or extrapolated (relevant for
  viewports wider than 640, and for the 2×2-quad granularity at the very edge);
* whether the interpolation is linear at all.

### Where the factor is applied

`gx.h:3294` says it *"is multiplied by the eye-space Z used for fog computation"*. In the
perspective formula `A/(B - zs) - C`, the term `A/(B - zs)` equals `ze/(end - start)` (§A.2) —
that, and not the final result, is the quantity proportional to eye-space Z. Multiplying there by
factor `k` yields `(k·ze - start)/(end - start)`, i.e. the range substituted for Z with `startz`
and `endz` still interpreted as distances, which is the behaviour the documentation describes.

So the order is: **reciprocal → multiply by the range factor → subtract C → apply the curve.**

**Unverified:** this insertion point is an inference from `gx.h:3294` plus the algebra, not a
quoted statement. Note also that on the orthographic path the corresponding term is
`(ze - near)/(end - start)` rather than `ze/(end - start)`, so a range multiply there does not
have a clean interpretation; range adjustment is a perspective feature in practice.

`GX_Init()` disables it: `GX_SetFogRangeAdj(GX_DISABLE,0,NULL)` (`gx.c:706`, and `gx.h:3299`).

## A.6 The fog colour, and where fog sits in the pipeline

Register `0xF2` carries **blue at 7:0, green at 15:8, red at 23:16** and no alpha
(`gx.c:4317`, and identically `GX_SetFogColor()` at `gx.c:4374`):

```c
val = 0xf2000000|(_SHIFTL(col.r,16,8))|(_SHIFTL(col.g,8,8))|(col.b&0xff);
```

`gx.h:3274-3275` describes it as *"in RGBX format (i.e. the alpha component is ignored)"* and as
*"the color of the pixel when fully fogged"*; `gx.h:3315` repeats that alpha is ignored. `GXColor`
is passed by value, so `col.a` is simply dropped.

**The blend.** The fog factor `f` selects between the TEV output colour and the fog colour:

```
rgb_out = rgb_tev * (1 - f) + rgb_fog * f
alpha_out = alpha_tev          (unchanged)
```

The endpoints are pinned by `gx.h:3275` (`f = 1` gives the fog colour) and by fsel 0 meaning no
fog (`f = 0` gives the TEV colour). **Unverified:** that the interpolation between those endpoints
is linear rather than some other monotone blend, and the fixed-point precision and rounding of the
multiply. Linear is the only form consistent with the linear fog type being called "linear", so
this is a low-risk assumption, but it is an assumption.

**Alpha is not fogged.** Register `0xF2` has no alpha field to blend towards, and every
description of the fog colour says alpha is ignored. Anything downstream that consumes alpha sees
the unmodified TEV alpha.

**Position in the pipeline.**

```
TEV stages  ->  Z texture  ->  fog range adjust + fog  ->  fog colour blend  ->  ...  ->  frame-buffer blend
                                                                             alpha test (on TEV alpha)
```

Evidence:

* `gx.h:3057` — *"The output pixel color (before fogging and blending) is the result from the last
  stage"*, placing fog **after all TEV stages** and **before the frame-buffer blend**.
* `gx.h:2778` — *"The Z-texture calculation is done before the fog range calculation"*, placing
  the Z-texture unit **before** fog.
* `gx.h:3074` and `gx.h:3080` — alpha compare *"uses the alpha output from the last active TEV
  stage"*. Since fog does not touch alpha, **the ordering of fog relative to the alpha test does
  not affect the result**: the alpha test sees TEV alpha either way.
* `gx.h:3529` — blending *"still use[s] source alpha (output from the last TEV stage)"*,
  confirming the blend's alpha input is the TEV alpha, not a fogged one.

**Unverified:** the physical ordering of the fog block and the alpha-compare block within the PE.
It is unobservable through the colour path (fog does not write alpha, the alpha test does not read
colour), so an emulator may place them in either order; but if you are matching a hardware
performance trace rather than pixels, do not treat the diagram above as settled.

---

# Part B — Indirect textures

## B.1 What an indirect stage is

An **indirect stage** is a texture lookup whose result is not a colour. Its texel is interpreted
as a vector of offsets that perturb the texture coordinate of a *later, ordinary* TEV stage. The
hardware has a separate small block of them, configured entirely independently of the TEV stage
array.

| | Indirect stage | Ordinary TEV stage |
|---|---|---|
| How many | 0–4 (`GX_MAX_INDTEXSTAGE` = 4, `gx.h:788`); count in genMode bits 18:16 | 1–16; count in genMode bits 13:10 |
| Count set by | `GX_SetNumIndStages()` (`gx.c:4101-4105`) | `GX_SetNumTevStages()` |
| Texture + coordinate | one register for all four stages, `0x27` (sibling §11.5) | two stages share a register, `0x28`–`0x2F` (sibling §7.3) |
| Produces | up to three offset components S, T, U, plus a "bump alpha" | a colour and an alpha, into `prev`/`reg0`/`reg1`/`reg2` |
| Consumed by | the texture-coordinate path of a TEV stage that names it | the next stage's combiner inputs |
| Configured per | stage id 0–3 (`GX_INDTEXSTAGE0..3` = 0..3, `gx.h:784-787`) | TEV stage 0–15 |

`GX_SetIndTexOrder()` (`gx.c:4548-4570`) assigns the texture map and texture coordinate, six bits
per stage, map before coordinate (sibling §11.5). `GX_SetIndTexCoordScale()` (`gx.c:4174-4198`)
sets a per-stage divisor applied **to the coordinate used for the indirect lookup only** —
`gx.h:865` is explicit that it applies to *"the texcoord used for an indirect initial lookup (not
a TEV stage regular lookup)"*. That is what lets one texgen feed both a small indirect map and a
large regular map.

The wiring from indirect stage to consumer is the other way round from what the name suggests:
**a TEV stage names an indirect stage**, in its own indirect-command register `0x10 + tevstage`
(sibling §11.1). Any number of TEV stages may name the same indirect stage; a TEV stage that wants
no indirection is set to `GX_SetTevDirect()` (`gx.c:4096-4099`), which writes an all-defaults
command:

```c
GX_SetTevIndirect(tevstage,GX_INDTEXSTAGE0,GX_ITF_8,GX_ITB_NONE,GX_ITM_OFF,
                  GX_ITW_OFF,GX_ITW_OFF,GX_FALSE,GX_FALSE,GX_ITBA_OFF);
```

Note this still names indirect stage 0 — the thing that disables indirection is
`GX_ITM_OFF` (matrix "off", *"a matrix of all zeroes"*, `gx.h:823`), which zeroes the offset.
`GX_Init()` issues `GX_SetTevDirect()` for every stage and then `GX_SetNumIndStages(0)`
(`gx.c:696-703`).

**One extra register to keep in sync.** The indirect texture mask (`0x0F`, sibling §11.3) is a
bitmask of which texture maps the indirect unit consumes. libogc recomputes it in
`__GX_UpdateBPMask()` (`gx.c:929-965`) as `nres |= (1<<ntexmap)` over the active indirect stages,
and — importantly — **this recomputation is compiled out on Wii** (`#if defined(HW_DOL)`,
`gx.c:931`). `GX_SetNumIndStages()` and `GX_SetIndTexOrder()` both mark the state dirty to force
it (`dirtyState |= 0x0006` at `gx.c:4104`, `|= 0x0003` at `gx.c:4569`).

## B.2 The indirect matrix

### Shape

Three static matrices, each **2 rows × 3 columns** — `f32 offset_mtx[2][3]` in
`GX_SetIndTexMatrix()` (`gx.c:4107`). They occupy nine BP registers `0x06`–`0x0E`, three per
matrix, and each register carries **one column**: `offset_mtx[0][col]` in bits 10:0 and
`offset_mtx[1][col]` in bits 21:11 (sibling §11.2). The sibling document's phrase "three rows of
two" describes the *register* arrangement — three registers each holding two elements. The
*mathematical* object is 2×3.

The multiply is stated by `gx.h:3343-3344`: *"The matrix is multiplied by the [S T U] offsets …
In this matrix-vector multiply, the matrix is on the left and the [S T U] column vector is on the
right."* So:

```
[ ds ]   [ m[0][0]  m[0][1]  m[0][2] ]   [ S ]
[    ] = [                            ] * [ T ]
[ dt ]   [ m[1][0]  m[1][1]  m[1][2] ]   [ U ]
```

Two rows because a texture coordinate has two components; three columns because the indirect
texel has three.

### Element format

`gx.c:4120-4121` (and the two analogous pairs at 4125-4126, 4130-4131):

```c
ma = (u32)(offset_mtx[0][0]*1024.0F);
mb = (u32)(offset_mtx[1][0]*1024.0F);
```

stored into 11-bit fields. `gx.h:3346-3347` gives the format directly: *"a sign and 10 fractional
bits (two's complement); thus the smallest number that can be stored is -1 and the largest is
(1 - 1/1024)"*. So each element is **s0.10 two's complement**: value = `stored_signed / 1024`,
range [−1, +0.9990234375]. **+1 is not representable**, and `gx.h:3347-3348` recommends halving
all elements and adding 1 to the scale exponent to work around it.

Note that `(u32)(x*1024.0F)` is a C float→unsigned conversion: for negative `x` this is undefined
behaviour in standard C, and in practice on PowerPC produces the two's-complement pattern that the
11-bit field wants. Reproduce the *field contents* (sign-extend bit 10), not libogc's cast.

### Scale exponent

`gx.c:4117`: `s = (scale_exp+17);` — a 6-bit biased value spread two bits at a time across the
matrix's three registers (bits 23:22 of each; column A carries `s` bits 1:0, column B `s>>2`,
column C `s>>4`; sibling §11.2). Stored 0–63 therefore corresponds to `scale_exp` −17…+46, though
nothing in libogc exercises the extremes.

The scale multiplies the matrix product by `2^scale_exp`. This is **provable from
`GX_SetTevIndTile()`** (`gx.c:4234-4245`), whose comment says so outright:

```c
/* Dividing the tile spacing by 2^10 and passing 10 as the exponent to
 * GX_SetIndTexMatrix() does not alter the end result, but allows us to
 * workaround the numeric limitations described in GX_SetIndTexMatrix()
 * documentation. */
offset_mtx[0][0] = ((f32)tilespacing_x)/(1 << 10);
...
GX_SetIndTexMatrix(indtexmtx,offset_mtx,10);
```

`spacing/2^10` with exponent 10 being interchangeable with `spacing` at exponent 0 is only true if
the result is `(matrix · vector) * 2^scale_exp`. The scale exists precisely because the elements
are confined to [−1, 1).

### Result magnitude

Follow the tiling case through. The tile-definition map's texel holds a tile index `k` (an 8-bit
value under `GX_ITF_8`). The offset produced is `(k * spacing/1024) * 2^10 = k * spacing`, which
is the correct offset **in texels of the regular map** to reach tile `k`. So:

> The indirect components enter the matrix multiply as **raw integers** (0–255 for `GX_ITF_8`,
> after bias), and `(M · [S T U]) * 2^scale_exp` is the coordinate offset **in texels** of the
> regular texture.

**Unverified:** the internal fixed-point alignment between that offset and the regular texture
coordinate as the SU unit carries it (GX texture coordinates reach the texture unit already scaled
by the map dimensions and with fractional bits below the texel). The *texel-unit* relation above
follows from the tiling derivation; the number of fractional bits the hardware shifts by does not.

### Dynamic S and T matrices

`GX_ITM_S0..S2` and `GX_ITM_T0..T2` select a "dynamic" matrix instead of a stored one. `gx.h:827,
830` calls them *"dynamic S-type matrix, indirect scale 0"* etc., and `gx.h:3368-3369` (the
`GX_SetTevIndBumpST` notes) is the key statement: *"Although a static indirect matrix is not used,
one must choose a matrix slot and set up the associated scale value to be used with this lookup."*

So a dynamic matrix **ignores the nine stored element bits but still uses the chosen slot's scale
exponent**. Its coefficients come from the incoming regular texture coordinate instead — which is
what allows a flat 2D bump map to be oriented onto the surface, since the geometry supplies
binormal and tangent texgens to the stages that use the S- and T-type matrices (`gx.h:3366-3368`).

**Unverified:** the exact coefficient matrix the S-type and T-type modes construct from the
incoming coordinate. `gx.c` never expresses it — libogc only selects the mode.

## B.3 The enumerations

### `GX_ITF_*` — indirect texture format (`0x10+n` bits 3:2)

`gx.h:796-799`. The number of bits taken from each component byte of the indirect texel.

| Value | Name | Offset bits per component | Bump-alpha bits |
|---|---|---|---|
| 0 | `GX_ITF_8` | 8 | byte is *duplicated* — see below |
| 1 | `GX_ITF_5` | 5 | 3 |
| 2 | `GX_ITF_4` | 4 | 4 |
| 3 | `GX_ITF_3` | 3 | 5 |

`gx.h:792-793` is the authority on the split: *"Bits for the indirect offsets are extracted from
the high end of each component byte. Bits for the bump alpha are extraced [sic] off the low end of
the byte. For `GX_ITF_8`, the byte is duplicated for the offset and the bump alpha."*

So the offset for `GX_ITF_5` is `byte >> 3`, for `GX_ITF_4` it is `byte >> 4`, for `GX_ITF_3` it is
`byte >> 5`; and the leftover low bits feed bump alpha. The format is chosen per *TEV stage*, not
per indirect stage — two TEV stages may read the same indirect result at different precisions.

**Unverified:** the exact right-alignment and width of the bump-alpha value in each format.
`gx.h:3041` states elsewhere that *"bump alpha contains only 5-bit data"*, which is consistent with
`GX_ITF_3` (5 low bits) being the widest case, but does not say how the narrower cases (3 and 4
bits) or the duplicated `GX_ITF_8` case are positioned within that 5-bit path.

**Unverified: which texel channel supplies S, T and U.** Neither source names it. `gx.h:3343-3344`
establishes the *vector* order ([S T U] as a column), and `GX_SetTevIndTile` establishes the
*column* order of the matrix (column 0 pairs with the first component, which carries the X tile
index), but the mapping from the sampled texel's red/green/blue to S/T/U is never written down in
either file. The obvious reading — first channel to S, second to T, third to U — is very likely
right, but treat it as an assumption to be hardware-tested.

### `GX_ITB_*` — bias select (`0x10+n` bits 6:4)

`gx.h:809-816`. Three independent enable bits, one per component: **bit 4 = S, bit 5 = T,
bit 6 = U**.

| Value | Name | S | T | U |
|---|---|---|---|---|
| 0 | `GX_ITB_NONE` | | | |
| 1 | `GX_ITB_S` | ● | | |
| 2 | `GX_ITB_T` | | ● | |
| 3 | `GX_ITB_ST` | ● | ● | |
| 4 | `GX_ITB_U` | | | ● |
| 5 | `GX_ITB_SU` | ● | | ● |
| 6 | `GX_ITB_TU` | | ● | ● |
| 7 | `GX_ITB_STU` | ● | ● | ● |

The bias *value* and its position in the sequence are stated by `gx.h:806`: *"The bias is fixed at
-128 for `GX_ITF_8` and +1 for the other formats. The bias happens prior to the indirect matrix
multiply."*

That is the mechanism that makes offsets **signed**. An 8-bit indirect texel is an unsigned 0–255;
biasing by −128 turns it into −128…+127 so the offset can move the coordinate in either direction.
The asymmetric +1 for the narrow formats is what the hardware does — do not "fix" it to a
half-range bias.

Order of operations, therefore: **extract → bias → matrix multiply → scale by 2^exp**.

### `GX_ITM_*` — matrix select (`0x10+n` bits 12:9)

`gx.h:823-832`. libogc writes this as one 4-bit field (`_SHIFTL(mtxid,9,4)`, `gx.c:4092`), but it
decomposes as **index in bits 10:9 and type in bits 12:11** (sibling §11.1), i.e.
`field = (type << 2) | index`:

| Value | Name | Type (12:11) | Index (10:9) | Meaning |
|---|---|---|---|---|
| 0 | `GX_ITM_OFF` | 0 | 0 | *"a matrix of all zeroes"* (`gx.h:823`) — offset is zero, indirection inert |
| 1 | `GX_ITM_0` | 0 | 1 | static matrix 0, scale 0 |
| 2 | `GX_ITM_1` | 0 | 2 | static matrix 1, scale 1 |
| 3 | `GX_ITM_2` | 0 | 3 | static matrix 2, scale 2 |
| 5 | `GX_ITM_S0` | 1 | 1 | dynamic S-type, scale 0 |
| 6 | `GX_ITM_S1` | 1 | 2 | dynamic S-type, scale 1 |
| 7 | `GX_ITM_S2` | 1 | 3 | dynamic S-type, scale 2 |
| 9 | `GX_ITM_T0` | 2 | 1 | dynamic T-type, scale 0 |
| 10 | `GX_ITM_T1` | 2 | 2 | dynamic T-type, scale 1 |
| 11 | `GX_ITM_T2` | 2 | 3 | dynamic T-type, scale 2 |

The numbering is exactly `(type << 2) | index` — `GX_ITM_S0` = 5 = `0b0101`, `GX_ITM_T0` = 9 =
`0b1001`. Values 4, 8, 12–15 are not defined by the API (index 0 with a non-zero type, and
type 3). **Unverified:** those seven encodings.

The matrix *slot* and the scale *slot* are the same slot — there is no way to use matrix 1 with
scale 2. `GX_SetIndTexMatrix()` normalises any of the three families back to a bare index before
computing the register number (`gx.c:4112-4118`):

```c
if(indtexmtx>=GX_ITM_0 && indtexmtx<=GX_ITM_2)       indtexmtx -= GX_ITM_0;
else if(indtexmtx>=GX_ITM_S0 && indtexmtx<=GX_ITM_S2) indtexmtx -= GX_ITM_S0;
else if(indtexmtx>=GX_ITM_T0 && indtexmtx<=GX_ITM_T2) indtexmtx -= GX_ITM_T0;
else indtexmtx = GX_ITM_OFF;
idx = indtexmtx * 3;
```

so passing `GX_ITM_S1` to `GX_SetIndTexMatrix` writes the elements *and scale* of slot 1.

### `GX_ITW_*` — regular-coordinate wrap (`0x10+n` bits 15:13 for S, 18:16 for T)

`gx.h:841-847`. This is applied to the **regular** texture coordinate of the consuming TEV stage,
before the indirect offset is added — not to the indirect lookup's own coordinate.

| Value | Name | Effect |
|---|---|---|
| 0 | `GX_ITW_OFF` | no wrap; the regular coordinate passes through |
| 1 | `GX_ITW_256` | wrap the regular coordinate to 256 |
| 2 | `GX_ITW_128` | wrap to 128 |
| 3 | `GX_ITW_64` | wrap to 64 |
| 4 | `GX_ITW_32` | wrap to 32 |
| 5 | `GX_ITW_16` | wrap to 16 |
| 6 | `GX_ITW_0` | **zero out the regular coordinate** |

`gx.h:838`: *"`GX_ITW_OFF` specifies no wrapping. `GX_ITW_0` will zero out the regular texture
coordinate."* Note the enumerators run *downwards* in size as the value rises, so the wrap modulus
is `256 >> (value - 1)` for values 1–5, and value 6 is the degenerate "modulus zero" case.

`GX_ITW_0` is how "replace the coordinate" is expressed: with the regular coordinate forced to 0,
the final coordinate is the indirect offset alone. Both convenience helpers that want replacement
semantics use it (§B.4).

S and T get independent 3-bit fields and are always written independently
(`_SHIFTL(wrap_s,13,3)|_SHIFTL(wrap_t,16,3)`, `gx.c:4092`), though every libogc helper happens to
set them to the same value.

**Unverified:** the units of the wrap modulus. The values are plainly texel counts of the regular
map, but whether the hardware wraps the integer texel part only or the full fixed-point coordinate
is not stated.

### `GX_ITS_*` — indirect coordinate scale (`0x25`, `0x26`, 4 bits per component)

`gx.h:870-878`, one 4-bit field for S and one for T per indirect stage (sibling §11.4).

| Value | Name | Effect on the indirect lookup's coordinate |
|---|---|---|
| 0 | `GX_ITS_1` | ÷1 |
| 1 | `GX_ITS_2` | ÷2 |
| 2 | `GX_ITS_4` | ÷4 |
| 3 | `GX_ITS_8` | ÷8 |
| 4 | `GX_ITS_16` | ÷16 |
| 5 | `GX_ITS_32` | ÷32 |
| 6 | `GX_ITS_64` | ÷64 |
| 7 | `GX_ITS_128` | ÷128 |
| 8 | `GX_ITS_256` | ÷256 |

i.e. the divisor is `2^value`. `gx.h:867`: *"The scale value is a fraction; thus `GX_ITS_32` means
to divide the texture coordinate values by 32."* Values 9–15 are undefined
(`GX_MAX_ITSCALE` = 9). `GX_Init()` sets all four stages to `GX_ITS_1`/`GX_ITS_1`
(`gx.c:700-703`).

Purpose (`gx.h:3255-3257`): to let one texgen serve both a large regular map and a smaller
indirect map, since the coordinate arriving from the texgen has already been scaled for the
regular map's dimensions.

## B.4 The convenience modes

These three helpers are the shapes real games emit, because they are the only documented recipes.
Recognising their exact register patterns is the fastest route to getting the common cases right.

### Bump mapping in S and T — `GX_SetTevIndBumpST()` (`gx.c:4136-4162`)

Takes **three consecutive TEV stages** to compute one lookup. `gx.h:3362-3364` describes the
purpose: the indirect map holds offsets in (S, T) space, and the three stages orient that flat 2D
bump map to the surface so it can perturb a normal-based texgen feeding an environment map.

```c
switch(mtx_sel) {
    case GX_ITM_0: sel_s = GX_ITM_S0; sel_t = GX_ITM_T0; break;
    case GX_ITM_1: sel_s = GX_ITM_S1; sel_t = GX_ITM_T1; break;
    case GX_ITM_2: sel_s = GX_ITM_S2; sel_t = GX_ITM_T2; break;
    default:       sel_s = GX_ITM_OFF; sel_t = GX_ITM_OFF; break;
}

GX_SetTevIndirect((tevstage+0),indstage,GX_ITF_8,GX_ITB_ST,sel_s,GX_ITW_0,GX_ITW_0,GX_FALSE,GX_FALSE,GX_ITBA_OFF);
GX_SetTevIndirect((tevstage+1),indstage,GX_ITF_8,GX_ITB_ST,sel_t,GX_ITW_0,GX_ITW_0,GX_TRUE, GX_FALSE,GX_ITBA_OFF);
GX_SetTevIndirect((tevstage+2),indstage,GX_ITF_8,GX_ITB_NONE,GX_ITM_OFF,GX_ITW_OFF,GX_ITW_OFF,GX_TRUE,GX_FALSE,GX_ITBA_OFF);
```

Field by field, in register `0x10 + tevstage`:

| Field | Stage +0 | Stage +1 | Stage +2 |
|---|---|---|---|
| ind stage (1:0) | `indstage` | `indstage` | `indstage` |
| format (3:2) | 0 (`ITF_8`) | 0 | 0 |
| bias (6:4) | 3 (`ITB_ST`: S and T biased by −128) | 3 | 0 (none) |
| bump alpha (8:7) | 0 (off) | 0 | 0 |
| matrix (12:9) | dynamic **S**-type, chosen slot (5/6/7) | dynamic **T**-type, chosen slot (9/10/11) | 0 (`ITM_OFF`) |
| wrap S (15:13) | 6 (`ITW_0`) | 6 | 0 (off) |
| wrap T (18:16) | 6 (`ITW_0`) | 6 | 0 (off) |
| utclod (19) | 0 | 0 | 0 |
| addprev (20) | 0 | **1** | **1** |

The pattern: the first two stages each zero the regular coordinate (`ITW_0`) so their output is
pure offset, apply the dynamic S- and T-type matrices respectively, and **accumulate** via
`addprev`; the third stage applies no indirection at all but keeps `addprev`, so it inherits the
accumulated coordinate and does the real texture lookup.

`gx.h:3366-3368` adds the constraints an emulator will see in the surrounding state: texture
lookup must be **disabled for the first two TEV stages** (via `GX_TEXMAP_DISABLE`, `gx.h:676`),
and the texgens must supply binormal to the first, tangent to the second, normal to the third.

`GX_SetTevIndRepeat()` (`gx.c:4169-4172`) is the companion for extending such a chain — it sets
`ITW_0`/`ITW_0` with `addprev = GX_TRUE`, no matrix, reusing the previous stage's computed
coordinate (`gx.h:3438, 3440`).

`GX_SetTevIndBumpXYZ()` (`gx.c:4164-4167`) is the one-stage object-space variant: a single
`GX_SetTevIndirect(tevstage,indstage,GX_ITF_8,GX_ITB_STU,mtx_sel,GX_ITW_OFF,GX_ITW_OFF,GX_FALSE,
GX_FALSE,GX_ITBA_OFF)` — all three components biased, a *static* matrix holding an object-to-
texture-space normal transform (`gx.h:3388-3389`), and no wrapping, so the offset perturbs rather
than replaces the coordinate.

### Warping — `GX_SetTevIndWarp()` (`gx.c:4200-4213`)

One stage. Two booleans collapse into two fields:

```c
u8 wrap = GX_ITW_OFF;
u8 bias = GX_ITB_NONE;
if(replace_tex) wrap = GX_ITW_0;
if(bias_flag)   bias = GX_ITB_STU;
GX_SetTevIndirect(tevstage, indtexid, GX_ITF_8, bias, mtxid, wrap, wrap, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
```

| Field | Value |
|---|---|
| format (3:2) | 0 (`ITF_8`) always |
| bias (6:4) | 7 (`ITB_STU`) if `bias_flag`, else 0 |
| bump alpha (8:7) | 0 |
| matrix (12:9) | the caller's `mtxid` (a static matrix in normal use) |
| wrap S (15:13), wrap T (18:16) | 6 (`ITW_0`) if `replace_tex`, else 0 (`ITW_OFF`) — **both always equal** |
| utclod (19), addprev (20) | 0, 0 |

`gx.h:3428-3429` gives the user-level meaning: `bias_flag` *"biases the indirect texture map
offsets by -128"* (matching the `GX_ITF_8` bias value in §B.3, so the warp can pull in both
directions); `replace_tex` makes the offsets *"replace the input texture coordinates rather than
offset them"* — which is `GX_ITW_0` zeroing the regular coordinate.

### Tiling — `GX_SetTevIndTile()` (`gx.c:4215-4247`)

One stage, plus a matrix write. The regular map holds the **tile definitions**; the indirect map
holds one texel per tile position, whose value is the tile index.

```c
wrap_s = GX_ITW_OFF;
if(tilesize_x==16) wrap_s = GX_ITW_16;
else if(tilesize_x==32)  wrap_s = GX_ITW_32;
else if(tilesize_x==64)  wrap_s = GX_ITW_64;
else if(tilesize_x==128) wrap_s = GX_ITW_128;
else if(tilesize_x==256) wrap_s = GX_ITW_256;
/* ...same for tilesize_y -> wrap_t... */

offset_mtx[0][0] = ((f32)tilespacing_x)/(1 << 10);
offset_mtx[0][1] = 0.0F;  offset_mtx[0][2] = 0.0F;
offset_mtx[1][0] = 0.0F;
offset_mtx[1][1] = ((f32)tilespacing_y)/(1 << 10);
offset_mtx[1][2] = 0.0F;

GX_SetIndTexMatrix(indtexmtx,offset_mtx,10);
GX_SetTevIndirect(tevstage,indtexid,indtexfmt,bias_sel,indtexmtx,wrap_s,wrap_t,GX_FALSE,GX_TRUE,alpha_sel);
```

What it sets:

| Field | Value |
|---|---|
| matrix registers `0x06+3m`..`0x08+3m` | a **diagonal** matrix: `[0][0] = spacing_x/1024`, `[1][1] = spacing_y/1024`, all other elements 0; scale exponent 10 (stored as 27) |
| format (3:2) | the caller's `indtexfmt` — not forced to `ITF_8` |
| bias (6:4) | the caller's `bias_sel`; `gx.h:3416` says this *"indicate[s] tile stacking direction for pseudo-3D textures"* |
| matrix (12:9) | the caller's `indtexmtx` |
| wrap S (15:13) | the tile width: 5/4/3/2/1 for 16/32/64/128/256, else 0 |
| wrap T (18:16) | the tile height, same mapping |
| **utclod (19)** | **1** |
| addprev (20) | 0 |
| bump alpha (8:7) | the caller's `alpha_sel` |

The two essential pieces:

* **The wrap fields carry the tile size**, so the regular coordinate is reduced modulo one tile —
  that is the position *within* a tile. The matrix turns the indirect texel into `index × spacing`,
  the base of the tile. Added together they address the right texel in the tile-definition map.
* **`utclod` is set**, i.e. mipmap LOD is computed from the *unmodified* coordinate. Necessary,
  because the wrapped coordinate is discontinuous at every tile boundary and its screen-space
  derivatives would be meaningless.

`gx.h:3403-3405` adds two caller obligations worth checking when reading a trace: the effective
texture size is `tile size × indirect map size`, so the regular texcoord scale must be set with
`GX_SetTexCoordScaleManually()`; and `GX_SetIndTexCoordScale()` is typically used so one texcoord
serves both lookups.

Note that a tile size that is not one of 16/32/64/128/256 silently falls through to `GX_ITW_OFF`
— no wrap at all — with no diagnostic.

## B.5 Bump alpha

### Selecting it

`GX_ITBA_*` (`gx.h:857-860`) is a 2-bit field at bits 8:7 of the indirect command
(`_SHIFTL(a,7,2)`, `gx.c:4092`):

| Value | Name | Meaning |
|---|---|---|
| 0 | `GX_ITBA_OFF` | no bump alpha from this stage |
| 1 | `GX_ITBA_S` | take bump alpha from the S component |
| 2 | `GX_ITBA_T` | from the T component |
| 3 | `GX_ITBA_U` | from the U component |

`gx.h:852` — *"Indicates which offset component should provide the 'bump' alpha output for the
given TEV stage."* And `gx.h:854` is a hardware restriction to honour: **"Bump alpha is not
available for TEV stage 0."**

### What it produces

The indirect texel's component byte is split (§B.3): the **high** bits become the offset, the
**low** bits become the bump alpha. So bump alpha is the residue of the same texel — free extra
data carried alongside the offsets, at the cost of offset precision.

It is consumed **as the rasterised-colour input of a TEV stage**, not as a texture input. The
rasterised-colour field of the TEV stage-order register (sibling §7.3) has dedicated encodings for
it. `gx.h:3040-3042`:

> *"If you specify `GX_ALPHA_BUMP` or `GX_ALPHA_BUMPN`, you can use 'Bump alpha' component from
> indirect texture unit as rasterized color input … Since bump alpha contains only 5-bit data,
> `GX_ALPHA_BUMP` shifts them to higher bits, which makes the value range 0-248. Meanwhile
> `GX_ALPHA_BUMPN` performs normalization and you can get the value range 0-255."*

So bump alpha is a **5-bit** quantity, and the two consumer modes differ in how it is widened to
8 bits: `GX_ALPHA_BUMP` shifts left by 3 (`v << 3`, giving 0, 8, 16 … 248 — never reaching 255),
`GX_ALPHA_BUMPN` replicates so the range covers 0–255. `248 = 31 × 8` confirms the 5-bit width.

### The hardware encoding of that channel — a correction to the sibling document

`GX_SetTevOrder()` maps the API's channel id through a table before writing bits 9:7 / 21:19
(`gx.c:4011, 4022`), and that table is (`gx.c:100`):

```c
static const u8 _gxtevcolid[9] = {0,1,0,1,0,1,7,5,6};
```

Indexed by `GX_COLOR0`=0, `GX_COLOR1`=1, `GX_ALPHA0`=2, `GX_ALPHA1`=3, `GX_COLOR0A0`=4,
`GX_COLOR1A1`=5, `GX_COLORZERO`=6, `GX_ALPHA_BUMP`=7, `GX_ALPHA_BUMPN`=8 (`gx.h:44-52`), the
**hardware** field values are:

| Hardware value | Meaning |
|---|---|
| 0 | Colour channel 0 (`GX_COLOR0`, `GX_ALPHA0`, `GX_COLOR0A0` all map here) |
| 1 | Colour channel 1 |
| 5 | Bump alpha, shifted (`GX_ALPHA_BUMP`) |
| 6 | Bump alpha, normalised (`GX_ALPHA_BUMPN`) |
| 7 | Zero (`GX_COLORZERO`) |

Values 2, 3, 4 are not produced by libogc. **Unverified:** those three encodings.

This refines sibling §7.3, which states that the field "takes the value `GX_ALPHA_BUMP` when the
stage's colour input is `GX_COLORNULL`". That is literally what `gx.c:4010` and `gx.c:4021` say —
`colid = GX_ALPHA_BUMP;` before the `if(color!=GX_COLORNULL)` override — but it is a **name
collision, not a semantic**: the constant `GX_ALPHA_BUMP` happens to equal 7, and hardware value 7
means *zero*, which is exactly what `_gxtevcolid[GX_COLORZERO]` also produces. A null colour input
rasterises as zero, not as bump alpha.

---

## C. Summary of what could not be determined

Ordered by how much damage a wrong guess does.

1. **The four exponential fog curves** (fsel 4, 5, 6, 7). Neither `gx.c` nor `gx.h` states them.
   The candidate forms in §A.4 are plausible and self-consistent but unproven. Highest-value
   hardware test in this document.
2. **The fog range-adjust index and interpolation** (§A.5). The table contents and their meaning
   are fully derived; how `|x - center|` is split into index and weight, what happens below 32
   pixels and above 320, and whether the blend is linear, are all inference.
3. **Which indirect texel channel supplies S, T and U** (§B.3). Never named in either file.
4. **The dynamic S-type and T-type matrix coefficients** (§B.2). libogc only selects the mode; the
   matrix the hardware synthesises from the incoming coordinate is not expressed anywhere.
5. **Bump-alpha bit positioning per format** (§B.3, §B.5). "Low end of the byte" plus "5-bit data"
   pins the concept but not the alignment for `GX_ITF_8`, `GX_ITF_5` and `GX_ITF_4`.
6. **Fixed-point alignment of the indirect offset against the regular coordinate** (§B.2). The
   texel-unit magnitude is derived; the number of fractional bits is not.
7. **Whether the wrap modulus applies to the integer texel part or the whole fixed-point
   coordinate** (§B.3).
8. **Fog coordinate clamping** (§A.3) — some clamp to [0, 1] must exist; its position and
   precision are unknown.
9. **The fog colour blend's linearity and rounding** (§A.6). The endpoints are certain.
10. **The exact insertion point of the range-adjust multiply** (§A.5) — inferred from `gx.h:3294`
    plus algebra, not quoted.
11. **Physical ordering of the fog block and the alpha-compare block** (§A.6). Unobservable
    through the colour path, so harmless for pixel accuracy.
12. **Integer rounding inside the fog reciprocal** (§A.2). The algebra is exact; the divider's
    fixed-point behaviour is not.
13. **Unused encodings.** Fog fsel 1 and 3 (§A.4); `GX_ITM_*` field values 4, 8, 12–15 (§B.3);
    `GX_ITS_*` values 9–15 (§B.3); TEV rasterised-colour values 2, 3, 4 (§B.5); IEEE exponent 0
    in the A and C fog registers (§A.1).

Two libogc source-level quirks that **must** be reproduced rather than corrected:

* the `8388638.0f` mantissa scale for fog B (`gx.c:4297`), which is not 2^23;
* the asymmetric indirect bias of −128 for `GX_ITF_8` and **+1** for the narrow formats
  (`gx.h:806`).

And one that must **not** be copied: `GX_SetTevIndTile()`'s silent fall-through to `GX_ITW_OFF`
for a tile size that is not a power of two in 16…256 (`gx.c:4220-4232`) is a library limitation,
not a hardware one.
