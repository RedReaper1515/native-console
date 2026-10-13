# The GameCube TEV — Texture Environment Unit

## Provenance

This document describes the *behaviour* of the Flipper TEV: the sixteen-stage colour combiner that
turns rasterised colour, texture samples, constants and previous-stage results into the pixel the
Pixel Engine finally writes.

It is a companion to [`gc-bp-registers.md`](gc-bp-registers.md), which records the **register
numbers and bit positions**. That information is *not* repeated here; every reference to a field
cites the relevant section of the sibling document instead. Read that first.

Sources, and how each was used:

* **`roms/gc/hwtests/gxtest/tev.cpp`** (Dolphin Emulator Project, GPL-2.0). This is a *hardware*
  test: it configures the real GPU with randomised combiner settings, reads the result back out of
  the EFB, and compares it against a reference model written by somebody who measured the console.
  The arithmetic in §2 is a restatement of that reference model, in prose and notation. Its
  randomised loop exercises roughly 61 000 random (a, b, c, scale, bias, op, clamp) combinations
  over the full 11-bit signed input range, so the arithmetic below is *measured*, not inferred. No
  code or comment text has been copied.
* **`roms/gc/hwtests/gxtest/BPMemory.h`** (GPL-2.0). Read for the enumerations only — the input
  selector numbering, the bias/op/scale encodings, the compare modes and the konstant selection
  table. Facts, not expression.
* **`scratchpad/gx.c`** (devkitPro/libogc, zlib — freely reusable). Line numbers refer to the
  5199-line copy used for this analysis. Quoted where it shows how the API arguments reach the
  registers.

Anything that neither the hardware test nor `gx.c` pins down is marked **Unverified:**.

Two conventions used throughout:

* `x >> n` always means an **arithmetic** shift right — the sign bit is replicated, so shifting a
  negative value right rounds toward negative infinity. This matters; see §2.3 and Example 3.
* `x & 255` means truncation to the low eight bits, discarding sign.

---

## 1. The stage pipeline

### 1.1 What a stage is

A TEV stage is one evaluation of **two independent combiners**: a *colour* combiner producing three
8-or-more-bit channels, and an *alpha* combiner producing one. They share a stage index, a
destination register selector, and the stage's texture/rasteriser bindings, but they have separate
input selectors, separate bias/op/scale/clamp controls, and separate destination fields.

A stage consumes:

| Source | Where it comes from |
|---|---|
| The four TEV colour registers | `prev`, `c0`, `c1`, `c2` — see §1.2 |
| One texture sample | The texture map / texture coordinate bound to this stage by the stage-order register ([`gc-bp-registers.md` §7.3](gc-bp-registers.md#73-tev-stage-order-0x280x2f)), passed through the stage's texture swap table (§7) |
| One rasterised colour | The colour channel selected by the same stage-order register, passed through the stage's rasteriser swap table (§7) |
| One konstant colour and one konstant alpha | Selected per stage out of the konstant registers and the fractional constants (§6) |
| Two built-in constants | `ONE` and `HALF`, colour combiner only |

and produces one RGBA value, written into one of the four TEV colour registers.

### 1.2 The colour registers and chaining

There are **four** TEV colour registers, each holding four channels of **signed 11-bit** data
(range −1024 … +1023). Their layout and register numbers are in
[`gc-bp-registers.md` §7.5](gc-bp-registers.md#75-tev-colour-and-konstant-registers-0xe00xe7).

| Index | Name | Role |
|---|---|---|
| 0 | `prev` | Both the default destination and the conventional carrier of the running result |
| 1 | `c0` | General purpose |
| 2 | `c1` | General purpose |
| 3 | `c2` | General purpose |

The same four indices are used by the destination field (bits 23:22 of both combiner registers) and
by the input selectors (§4, §5). Register 0 is *not* special in the datapath — a stage may read
from and write to any of the four — but it *is* special at the end: **the value handed to the Pixel
Engine is the contents of `prev` after the last enabled stage.**

> Evidence: `gxtest`'s `GetTevOutput` helper, when the stage under test writes to `c0`/`c1`/`c2`,
> appends extra TEV stages whose only purpose is to move that register's contents into `prev`
> before the EFB copy. If the PE read the last stage's destination, that plumbing would be
> pointless.

Chaining is therefore entirely by convention plus the register file: stage *n* writes a register,
stage *n+1* names that register as one of its inputs. There is no implicit forwarding. A useful
consequence, and a trap, is documented in §2.3: a register written *unclamped* can hold values
outside 0…255, and a later stage that reads it as `a`, `b` or `c` will see it **truncated modulo
256**.

There is one measured oddity. The `gxtest` author notes that `prev` as an input to **stage 0** does
not appear to read back what was written into the `prev` register beforehand. **Unverified:** the
test does not establish what stage 0 actually reads in that case — it works around the behaviour
rather than characterising it. An implementation should not rely on priming `prev` before the first
stage.

### 1.3 Stage count

`GenMode` bits 13:10 hold **the number of TEV stages minus one**
([`gc-bp-registers.md` §2](gc-bp-registers.md#2-genmode-0x00)). A field value of 0 means one stage
is evaluated (stage 0); a value of 15 means sixteen (stages 0…15). Stages are evaluated in
ascending index order, and stages above the count are not evaluated at all — their registers retain
whatever was last written to them and have no effect.

The mapping is exercised directly by the hardware tests: they write `numtevstages = 0` for
single-stage tests and `numtevstages = 1` for the two-stage konstant test.

`GX_SetNumTevStages()` performs the conversion in software (`gx.c:4035`):

```c
__gx->genMode = (__gx->genMode&~0x3C00)|(_SHIFTL((num-1),10,4));
```

### 1.4 Rasterised colour channel selection

The stage-order register's 3-bit colour-channel field (bit positions in
[`gc-bp-registers.md` §7.3](gc-bp-registers.md#73-tev-stage-order-0x280x2f)) decides what `ras`
means for the stage:

| Value | Meaning |
|---|---|
| 0 | Lit colour channel 0 |
| 1 | Lit colour channel 1 |
| 2, 3, 4 | **Unverified:** not named by `BPMemory.h`, never emitted by libogc |
| 5 | Alpha bump (from the indirect unit) |
| 6 | Normalised alpha bump |
| 7 | Zero |

Cross-checked against libogc's translation table `static const u8 _gxtevcolid[9] = {0,1,0,1,0,1,7,5,6};`
(`gx.c:100`), which maps the nine API channel constants onto these hardware values. Its shape
corroborates the table above: six entries collapse onto the two lit channels 0 and 1, and the last
three produce 7, 5 and 6. **Unverified:** which API constant occupies which index — `gx.h` was not
part of this analysis — though the conventional GX ordering (`GX_COLOR0`, `GX_COLOR1`, `GX_ALPHA0`,
`GX_ALPHA1`, `GX_COLOR0A0`, `GX_COLOR1A1`, `GX_COLORZERO`, `GX_ALPHA_BUMP`, `GX_ALPHA_BUMPN`) fits
it exactly.

One libogc quirk worth noting: when the API channel is `GX_COLORNULL`, libogc writes the constant
`GX_ALPHA_BUMP` into the field **without** passing it through `_gxtevcolid[]`
(`gx.c:4008`, `gx.c:4017`), so the raw API number reaches the hardware field. **Unverified:** what
hardware value that produces, for the same reason.

---

## 2. The colour combiner, exactly

### 2.1 The four inputs and their widths

Each stage names four inputs by 4-bit selector fields (`a` at bits 15:12, `b` at 11:8, `c` at 7:4,
`d` at 3:0 — [`gc-bp-registers.md` §7.1](gc-bp-registers.md#71-tev-colour-combiner-0xc0-2n)).
The selectors themselves are tabulated in §4. What matters for the arithmetic is that **the four
inputs are not treated alike**:

| Input | Role | Width entering the datapath |
|---|---|---|
| `a` | Interpolation endpoint at weight 0 | **Truncated to 8 bits**, unsigned: `a ← a & 255` |
| `b` | Interpolation endpoint at weight 1 | **Truncated to 8 bits**, unsigned: `b ← b & 255` |
| `c` | Interpolation weight | **Truncated to 8 bits**, unsigned: `c ← c & 255`, then widened (§2.2) |
| `d` | Additive term | **Full width, signed.** Not masked, not clamped. Carries the register's whole 11-bit signed value, −1024 … +1023 |

This asymmetry is the origin of most TEV emulation bugs. Only `d` can carry a value outside
0…255 into the arithmetic. `a`, `b` and `c` are reduced modulo 256 with no sign extension: a
register holding −1 presents as 255, a register holding −300 presents as 212, a register holding
+300 presents as 44.

### 2.2 Widening the interpolation weight

The 8-bit weight `c` must be able to express a weight of exactly 1, which needs the value 256. The
hardware widens it by adding its own top bit:

> **c′ = c + (c ≫ 7)** , equivalently **c′ = c + (c ≥ 128 ? 1 : 0)**

so the mapping 0…255 → 0…256 is:

| `c` | `c′` |
|---|---|
| 0 … 127 | unchanged, 0 … 127 |
| 128 … 255 | `c + 1`, i.e. 129 … 256 |

`c′` spans 0…256 inclusive, but it is **not onto**: the value 128 is the one output that no input
produces, so there is no weight of exactly one half. In particular the built-in `HALF` constant
(selector 13, value 128) widens to **129/256**, not 128/256, so `lerp(a, b, HALF)` is *not* a
symmetric average. See Example 4.

### 2.3 The order of operations

This is the whole difficulty. Let

* `s` = the **left** shift implied by the scale field: 0 for ×1, 1 for ×2, 2 for ×4, **0 for ÷2**
* `r` = the **right** shift implied by the scale field: 1 for ÷2, otherwise 0
* `σ` = +1 for op = Add, −1 for op = Sub
* `β` = the rounding bias: **0 if scale is ÷2**, otherwise 127 for Sub and 128 for Add
* `γ` = the bias term, scaled: 0 for bias = 0, +128 for bias = +½, −128 for bias = −½

Then, in order:

1. **Truncate.** `a ← a & 255`, `b ← b & 255`, `c ← c & 255`. `d` is left alone.
2. **Widen the weight.** `c′ ← c + (c >> 7)`, giving 0 … 256.
3. **Interpolate, without dividing yet.** `L ← a · (256 − c′) + b · c′`.
   `L` is non-negative and at most 255 · 256 = 65280, i.e. 17 unsigned bits.
4. **Apply the scale shift to the interpolation product, *before* rounding.** `S ← L ≪ s`.
   At most 19 bits.
5. **Add the rounding bias and only now divide by 256.** `R ← (S + β) ≫ 8`.
   Because `S ≥ 0`, this shift is an ordinary unsigned division; the sign of the shift does not
   matter here.
6. **Apply the scale shift to `d` as well, and combine with the op.**
   `T ← (d ≪ s) + σ · R`.
   Note `d` is shifted, not multiplied through the lerp, and the subtraction negates *the
   interpolation result only*, never `d`.
7. **Add the bias, itself scaled.** `U ← T + (γ ≪ s)`.
   So a bias of −½ at scale ×4 contributes −512, not −128.
8. **Apply the ÷2 as an arithmetic right shift, last.** `V ← U ≫ r`.
   `U` may be negative here, and the shift is arithmetic, so this **floors toward −∞**: −173 ≫ 1
   is −87, not −86.
9. **Clamp.**
   * If the stage's clamp bit (bit 19) is **set**: clamp to **0 … 255**.
   * If it is **clear**: clamp to **−1024 … +1023** — the 11-bit signed range of the destination
     register. This is a clamp, not a truncation: a result of +2000 stores as +1023, not as −48.

Compactly, with the shifts exactly where the numbered steps place them:

```
c′ = c + (c >> 7)
R  = ( ((a·(256 − c′) + b·c′) << s) + β ) >> 8        [>> 8 of a non-negative value]
U  = (d << s)  +  σ·R  +  (γ << s)
V  = clamp( U >> r )                                  [>> r arithmetic, U may be negative]
```

### 2.4 Consequences worth stating separately

* **The rounding bias is *not* applied when the scale is ÷2.** All three of `s = 0`, `β = 0` and
  `r = 1` hold together in that case. An implementation that treats ÷2 as "scale 1, then halve"
  while keeping `β = 128` will be off by one on roughly half of all inputs.
* **The Sub bias is 127, not 128.** The interpolation result is rounded with +127 and *then*
  negated, which is not the same as rounding the negated value with +128. Concretely, for
  `S = 38015`: Add would give (38015+128) ≫ 8 = 149, whereas Sub gives −((38015+127) ≫ 8) = −148.
* **Sub does not negate `d` or the bias.** Only the interpolation term changes sign.
* **The scale multiplies `d` and the bias too** — every term is scaled, because the shift is
  applied to each of them separately rather than to the finished sum. (The `÷2` is the exception:
  it *is* applied to the finished sum.)
* **Unclamped output feeds the next stage truncated.** A stage that writes, say, −204 into `c0`
  with the clamp bit clear stores −204. If the next stage reads `c0` as `d`, it sees −204. If it
  reads it as `a`, `b` or `c`, it sees `(−204) & 255 = 52`. Both are correct hardware behaviour.
* **The TEV output narrows to 8 bits at the Pixel Engine**, so the extra range exists only to be
  carried between stages. The `gxtest` harness has to shift the value down through extra TEV
  stages across multiple render passes precisely because the EFB cannot show all 11 bits.

### 2.5 Compare mode

When the bias field holds the value **3**, the stage is not an interpolation at all — it is a
comparison. The op bit (18) and the scale field (21:20) are reinterpreted; see
[`gc-bp-registers.md` §7.1](gc-bp-registers.md#71-tev-colour-combiner-0xc0-2n) for the bit
positions and for libogc's folding of the API op number into those bits.

| Bit 18 | Comparison |
|---|---|
| 0 | greater-than |
| 1 | equal-to |

| Bits 21:20 | Colour combiner | Alpha combiner |
|---|---|---|
| 0 | R8 — compare the red channels as 8-bit values | same |
| 1 | GR16 — compare `(green ≪ 8) \| red` as 16-bit values | same |
| 2 | BGR24 — compare `(blue ≪ 16) \| (green ≪ 8) \| red` as 24-bit values | same |
| 3 | RGB8 — compare each channel independently, 8 bits each | A8 — compare the alpha channels as 8-bit values |

The result has the shape

> **out = d + (compare(A, B) ? c : 0)**

with `d` and `c` fetched by the ordinary selectors, and for RGB8/A8 the comparison and the select
performed per channel.

The R8, greater-than case is directly measured: `gxtest` sets up `c0.r = 127`, `c1.r = 127`,
`c2.r = 127` or `128`, `c2.a = 255`, and confirms the stage outputs 255 exactly when the first
operand exceeds the second, 0 otherwise.

**Unverified:** the following are taken from `BPMemory.h`'s enumeration names and from the register
layout, not from measurement — the `gxtest` reference model explicitly declines to model compare
mode.

* Whether the comparison operands are truncated to 8 bits per channel before concatenation (almost
  certainly yes, by analogy with `a` and `b` in §2.1, but untested).
* Whether the comparison is unsigned.
* Whether the clamp bit still applies to the sum, and with which range.
* The GR16 and BGR24 channel orderings above (red in the low bits) follow the enumeration names
  `GR16` and `BGR24` read most-significant-first; this is a reading of a name, not a measurement.

---

## 3. The alpha combiner

The alpha combiner runs the arithmetic of §2.3 **unchanged** on a single channel. Every difference
is structural.

1. **Three-bit selectors, eight inputs.** The colour combiner has 4-bit selector fields and sixteen
   inputs; the alpha combiner has 3-bit fields and eight (§5). The fields sit at different bit
   positions as a result — `a` at 15:13, `b` at 12:10, `c` at 9:7, `d` at 6:4
   ([`gc-bp-registers.md` §7.2](gc-bp-registers.md#72-tev-alpha-combiner-0xc1-2n)).
2. **No `ONE`, no `HALF`.** The colour input list carries two built-in numeric constants; the alpha
   list carries only `konst` and `ZERO`. There is no way to feed a literal 255 or 128 into the alpha
   combiner without using a konstant or a register.
3. **No cross-channel inputs.** Colour selectors come in `.rgb` / `.aaa` pairs — the colour
   combiner can broadcast a register's alpha across all three colour channels. The alpha combiner
   has only the alpha of each register; it cannot read a colour channel through its own selectors.
4. **The bottom four bits of the register are not inputs.** Where the colour combiner's bits 3:0
   are input `d`, the alpha combiner's bits 3:0 are the two **swap-table selectors** (§7) — bits
   1:0 for the rasterised colour, bits 3:2 for the texture colour. These are per-stage settings
   that govern *both* combiners of the stage, but they physically live in the alpha register.
   `GX_SetTevSwapMode()` writes them (`gx.c:4068-4074`) and has nothing to do with alpha.
5. **Konstant selector values 12…15 are invalid for alpha.** Those four select whole RGB triples
   from the konstant registers, which the alpha datapath cannot use. For the alpha selector they
   read as 0, joining 8…11, so **8…15 all give 0** for alpha while only 8…11 do for colour (§6).
   Measured.
6. **In compare mode, the alpha combiner takes its comparison operands from the *colour*
   combiner's `a` and `b` selectors.** This is the least obvious fact in the whole unit, and
   `gxtest` tests it deliberately: with the alpha combiner set to compare `alpha0` against
   `alpha1` and the colour combiner set to `c2` against `c1`, the measured result follows `c2`
   versus `c1`. The alpha combiner's own `a` and `b` fields are ignored. This is necessary for the
   R8 / GR16 / BGR24 modes — those compare colour channels, which only the colour datapath has
   fetched — and the test does not distinguish whether A8 mode behaves the same way.
   **Unverified:** whether A8 compare mode also reads `a` and `b` from the colour combiner, or
   whether it uses the alpha combiner's own selectors. Also **Unverified:** whether `c` in alpha
   compare mode comes from the alpha combiner (the test's setup is consistent with it doing so, as
   the selected konstant register's alpha was the value observed, but `c` was not varied
   independently).
7. **The destination is the alpha channel** of the register named by bits 23:22, which is an
   independent field from the colour combiner's destination. A stage may perfectly well write its
   colour to `c1` and its alpha to `prev`.

Bias, op, clamp, scale and their bit positions are identical to the colour combiner, and so are the
clamp ranges — 0…255 clamped, −1024…+1023 unclamped.

---

## 4. Colour input selectors

The 4-bit value in each of the colour combiner's `a`, `b`, `c`, `d` fields. All four fields use
this same table.

| Value | Name | Reads |
|---|---|---|
| 0 | `PrevColor` | The RGB of the `prev` register — per-channel |
| 1 | `PrevAlpha` | The alpha of `prev`, broadcast to all three channels |
| 2 | `Color0` | The RGB of register `c0` |
| 3 | `Alpha0` | The alpha of `c0`, broadcast |
| 4 | `Color1` | The RGB of register `c1` |
| 5 | `Alpha1` | The alpha of `c1`, broadcast |
| 6 | `Color2` | The RGB of register `c2` |
| 7 | `Alpha2` | The alpha of `c2`, broadcast |
| 8 | `TexColor` | The RGB of this stage's texture sample, **after** the stage's texture swap table (§7) |
| 9 | `TexAlpha` | The alpha of the swapped texture sample, broadcast |
| 10 | `RasColor` | The RGB of this stage's rasterised colour, **after** the stage's rasteriser swap table (§7) |
| 11 | `RasAlpha` | The alpha of the swapped rasterised colour, broadcast |
| 12 | `One` | The constant 255 in every channel |
| 13 | `Half` | The constant 128 in every channel |
| 14 | `Konst` | The RGB selected by this stage's konstant colour selector (§6) |
| 15 | `Zero` | The constant 0 |

Notes.

* Values 0…7 form a regular pattern: `(index ≫ 1)` picks the register in the order prev, c0, c1,
  c2, and the low bit picks colour-per-channel (0) or alpha-broadcast (1).
* `Half` **is exactly 128** — measured directly: `gxtest` feeds `Half` into `d` with everything else
  zero and reads 128 back out of the EFB.
* **Unverified:** `One` = 255 is inferred, not measured by these tests. It is the only value
  consistent with the weight widening of §2.2 (255 widens to 256, an exact weight of one) and with
  the 8-bit convention everywhere else in the unit.
* As an interpolation weight `c`, the useful identities are `Zero` → weight 0 (result is `a`),
  `One` → weight 256 (result is `b`), `Half` → weight **129**, not 128.
* Remember §2.1: whichever of these lands in `a`, `b` or `c` is truncated to 8 bits first. For
  selectors 8…15 that truncation is a no-op, since none of them can exceed 255. It is only the
  register selectors 0…7 that can be affected.

libogc's `GX_CC_*` constants are these values unmodified — `GX_SetTevColorIn()` masks each argument
to 4 bits and shifts it into place with no translation table (`gx.c:3871-3880`).

---

## 5. Alpha input selectors

The 3-bit value in each of the alpha combiner's `a`, `b`, `c`, `d` fields.

| Value | Name | Reads |
|---|---|---|
| 0 | `PrevAlpha` | The alpha of the `prev` register |
| 1 | `Alpha0` | The alpha of register `c0` |
| 2 | `Alpha1` | The alpha of register `c1` |
| 3 | `Alpha2` | The alpha of register `c2` |
| 4 | `TexAlpha` | The alpha of this stage's texture sample, after the texture swap table (§7) |
| 5 | `RasAlpha` | The alpha of this stage's rasterised colour, after the rasteriser swap table (§7) |
| 6 | `Konst` | The value selected by this stage's konstant alpha selector (§6) |
| 7 | `Zero` | The constant 0 |

There is no `One` and no `Half`. Values 0…3 follow the same register order as the colour table's
0…7, minus the colour/alpha alternation.

`GX_SetTevAlphaIn()` likewise passes `GX_CA_*` through untranslated (`gx.c:3882-3891`).

---

## 6. Konstant selection

Each stage has **two independent 5-bit selectors**: one choosing its konstant *colour* (an RGB
triple, used when a colour input selector is 14) and one choosing its konstant *alpha* (a scalar,
used when an alpha input selector is 6). Both live in the shared konstant-select / swap-table
register file at `0xF6`…`0xFD`; bit positions are in
[`gc-bp-registers.md` §7.4](gc-bp-registers.md#74-tev-konstant-select--swap-table-0xf60xfd).

The konstant registers `k0`…`k3` themselves are the four 11-bit-signed RGBA registers that share
register numbers `0xE0`…`0xE7` with the TEV colour registers, discriminated by bit 23
([`gc-bp-registers.md` §7.5](gc-bp-registers.md#75-tev-colour-and-konstant-registers-0xe00xe7)).

### 6.1 The fractional constants (values 0…7)

The first eight selector values produce a numeric constant rather than a register read. **The exact
8-bit values are measured**, and they are not what a naive `(8−n)·32` or `(8−n)·255/8` truncation
would give:

| Value | Nominal | Exact 8-bit value | Hex |
|---|---|---|---|
| 0 | 1 | **255** | `0xFF` |
| 1 | 7/8 | **223** | `0xDF` |
| 2 | 3/4 | **191** | `0xBF` |
| 3 | 5/8 | **159** | `0x9F` |
| 4 | 1/2 | **128** | `0x80` |
| 5 | 3/8 | **96** | `0x60` |
| 6 | 1/4 | **64** | `0x40` |
| 7 | 1/8 | **32** | `0x20` |

The generating rule is **round-half-up of 255·(8−n)/8**:

> value(n) = ⌊ (255·(8 − n) + 4) / 8 ⌋

which reproduces every entry: 8/8 → 255, 7/8 → 223.125 → 223, 6/8 → 191.25 → 191, 5/8 → 159.375 →
159, 4/8 → **127.5 → 128**, 3/8 → 95.625 → 96, 2/8 → 63.75 → 64, 1/8 → 31.875 → 32.

The half-way case is the one that catches implementations out. `1/2` is **128**, not 127, and the
descending sequence therefore steps 255, 223, 191, 159, **128**, 96, 64, 32 — a gap of 31 between
values 3 and 4 where every other gap is 32. Implementations that use `255 − 32n` (wrong from n = 4
on) or `256 − 32n` clamped (wrong for n = 0…3) will both be off by one over half the range.

These eight values apply identically to the **colour** selector (all three channels take the same
value) and to the **alpha** selector.

### 6.2 Invalid values

| Value | Colour selector | Alpha selector |
|---|---|---|
| 8, 9, 10, 11 | Invalid — reads as 0 | Invalid — reads as 0 |
| 12, 13, 14, 15 | Valid, see §6.3 | **Invalid — reads as 0** |

Measured: the hardware test walks all 32 values of both selectors and observes 0 for colour
selectors 8…11 and for alpha selectors 8…15.

### 6.3 Whole-triple selections (values 12…15, colour only)

| Value | Selects |
|---|---|
| 12 | `k0.rgb` — the RGB of konstant register 0, per channel |
| 13 | `k1.rgb` |
| 14 | `k2.rgb` |
| 15 | `k3.rgb` |

Invalid for the alpha selector (§6.2), because a triple has no meaning for a scalar datapath.

### 6.4 Single-channel selections (values 16…31)

Valid for **both** the colour and the alpha selector. For the colour selector the chosen channel is
broadcast to all three output channels; for the alpha selector it is used as the scalar.

| Value | Selects | | Value | Selects |
|---|---|---|---|---|
| 16 | `k0.r` | | 24 | `k0.b` |
| 17 | `k1.r` | | 25 | `k1.b` |
| 18 | `k2.r` | | 26 | `k2.b` |
| 19 | `k3.r` | | 27 | `k3.b` |
| 20 | `k0.g` | | 28 | `k0.a` |
| 21 | `k1.g` | | 29 | `k1.a` |
| 22 | `k2.g` | | 30 | `k2.a` |
| 23 | `k3.g` | | 31 | `k3.a` |

The ordering is **channel-major, register-minor**: the register index is the low two bits, the
channel is the next two. The hardware test confirms all sixteen by loading `k`*i* with
r = 10 + 50*i*, g = 20 + 50*i*, b = 30 + 50*i*, a = 40 + 50*i* and reading back
10/60/110/160 for the reds, 20/70/120/170 for the greens, 30/80/130/180 for the blues and
40/90/140/190 for the alphas — in that register order, for both the colour and the alpha selector.

### 6.5 Software view

`GX_SetTevKColorSel()` and `GX_SetTevKAlphaSel()` (`gx.c:4046-4066`) each derive the register as
`tevstage / 2` and then write a 5-bit field whose position depends on the stage's parity. They apply
no translation: the `GX_TEV_KCSEL_*` / `GX_TEV_KASEL_*` API constants are these hardware values.

---

## 7. The swap tables

### 7.1 What they do

A swap table is a **four-component channel permutation** applied to a colour before it enters the
combiner. Each of its four outputs — the R, G, B and A the combiner will see — independently names
which of the *input's* four channels it takes:

> out.R = in[sel<sub>R</sub>], out.G = in[sel<sub>G</sub>], out.B = in[sel<sub>B</sub>],
> out.A = in[sel<sub>A</sub>]

Each `sel` is a **2-bit** field:

| Value | Source channel |
|---|---|
| 0 | Red |
| 1 | Green |
| 2 | Blue |
| 3 | Alpha |

Because each output independently names a source, the table is a general mapping, not a
permutation in the strict sense — duplication (`R,R,R,R`) and dropping channels are both legal and
both used. The identity table is `0, 1, 2, 3`.

### 7.2 Where they live

There are **four** swap tables, indices 0…3, shared by all sixteen stages. Each occupies two
registers in the `0xF6`…`0xFD` file, two 2-bit fields per register
([`gc-bp-registers.md` §7.4](gc-bp-registers.md#74-tev-konstant-select--swap-table-0xf60xfd)):

| Table | Register | Bits 1:0 | Bits 3:2 |
|---|---|---|---|
| *s* | `0xF6 + 2s` | `sel`<sub>R</sub> | `sel`<sub>G</sub> |
| *s* | `0xF6 + 2s + 1` | `sel`<sub>B</sub> | `sel`<sub>A</sub> |

`GX_SetTevSwapModeTable()` (`gx.c:4076-4087`) writes exactly this: `r` and `g` into the even
register, `b` and `a` into the odd one, each masked to two bits.

Note that these four bits are the *only* part of registers `0xF6`…`0xFD` that belongs to the swap
tables; bits 23:4 of the same registers are konstant selectors for a completely unrelated pair of
stages (§6). A write that touches one must preserve the other — libogc keeps a shadow word per
register precisely for this reason.

### 7.3 Where they apply

Each **stage** chooses two of the four tables, via two 2-bit fields at the bottom of its **alpha**
combiner register ([`gc-bp-registers.md` §7.2](gc-bp-registers.md#72-tev-alpha-combiner-0xc1-2n)):

| Bits | Selects the table applied to |
|---|---|
| 1:0 | The **rasterised** colour for this stage |
| 3:2 | The **texture** colour for this stage |

Written by `GX_SetTevSwapMode()` (`gx.c:4068-4074`), which despite editing the alpha register
affects both combiners.

The swap happens **between the fetch and the input multiplexer**, so:

* Input selectors `TexColor` (8), `TexAlpha` (9), `RasColor` (10), `RasAlpha` (11) in the colour
  combiner, and `TexAlpha` (4), `RasAlpha` (5) in the alpha combiner, all read the **post-swap**
  value.
* `TexAlpha` therefore means "whatever channel the stage's texture swap table routes to A" — which
  is how a program gets a texture's red channel into the alpha combiner, or samples a single
  channel of an RGBA texture per stage.
* Nothing else is swapped. The konstant selectors, the four colour registers, and the `One` /
  `Half` constants are untouched by swap tables.

**Unverified:** whether the swap is applied once per stage to a shared fetch or independently per
input selector is not observable — the two are equivalent, since a stage has a single texture
sample, a single rasterised colour, and a single table selection for each.

---

## 8. Worked examples

All of these use the colour combiner; the alpha combiner would behave identically on a single
channel. Each shows every intermediate of §2.3 so an implementation can be stepped against it.

### Example 1 — plain modulate

The classic `GX_MODULATE` setup: `a = ZERO`, `b = TexColor`, `c = RasColor`, `d = ZERO`, op = Add,
bias = 0, scale = ×1, clamp = on. Take the red channel with texture red = 200 and rasterised
red = 128.

| Step | Value |
|---|---|
| Truncate | a = 0, b = 200, c = 128 |
| Widen | c′ = 128 + (128 ≫ 7) = **129** |
| Interpolate | L = 0·(256−129) + 200·129 = **25 800** |
| Scale shift (s = 0) | S = 25 800 |
| Round and divide | R = (25 800 + 128) ≫ 8 = 25 928 ≫ 8 = **101** |
| Combine | T = (0 ≪ 0) + (+1)·101 = 101 |
| Bias | U = 101 + 0 = 101 |
| ÷2 shift (r = 0) | V = 101 |
| Clamp (on) | **101** |

For comparison, exact real arithmetic gives 200 · 128/255 = 100.39. The hardware's 101 comes from
the weight being 129/256 rather than 128/255 and from the +128 rounding bias — a two-step
discrepancy that a floating-point implementation will not reproduce.

### Example 2 — subtract, ×2 scale, −½ bias, unclamped

`a = 32`, `b = 200`, `c = 64`, `d = 100` (read from `c0`, so full width), op = **Sub**,
bias = **−½**, scale = **×2**, clamp = **off**.

| Step | Value |
|---|---|
| Truncate | a = 32, b = 200, c = 64 |
| Widen | c′ = 64 + (64 ≫ 7) = 64 + 0 = **64** |
| Interpolate | L = 32·(256−64) + 200·64 = 6144 + 12 800 = **18 944** |
| Scale shift (s = 1) | S = 18 944 ≪ 1 = **37 888** |
| Round (β = **127**, because op = Sub) and divide | R = (37 888 + 127) ≫ 8 = 38 015 ≫ 8 = **148** |
| Combine (σ = −1) | T = (100 ≪ 1) − 148 = 200 − 148 = **52** |
| Bias (γ = −128, scaled by s = 1) | U = 52 + (−128 ≪ 1) = 52 − 256 = **−204** |
| ÷2 shift (r = 0) | V = −204 |
| Clamp (**off**) | clamp to −1024 … 1023 → **−204** |

Three things to check an implementation against here: `d` was shifted left by 1 along with
everything else; the bias contributed −256, not −128; and the rounding bias was 127 (had it been
128, R would have been 149 and the answer −205).

If a later stage reads this `c0` as its `d`, it sees −204. If it reads it as `a`, `b` or `c`, it
sees `(−204) & 255 = 52`.

### Example 3 — divide-by-two, negative result, no rounding bias

`a = 0`, `b = 255`, `c = 255`, `d = −300` (from a register written with `GX_SetTevColorS10`),
op = Add, bias = **−½**, scale = **÷2**, clamp = **off**.

| Step | Value |
|---|---|
| Truncate | a = 0, b = 255, c = 255 |
| Widen | c′ = 255 + (255 ≫ 7) = 255 + 1 = **256** |
| Interpolate | L = 0·(256−256) + 255·256 = **65 280** |
| Scale shift (s = **0**, ÷2 does not left-shift) | S = 65 280 |
| Round (β = **0**, because scale is ÷2) and divide | R = (65 280 + 0) ≫ 8 = **255** |
| Combine (σ = +1) | T = (−300 ≪ 0) + 255 = **−45** |
| Bias (γ = −128, scaled by s = 0) | U = −45 − 128 = **−173** |
| ÷2 shift (r = **1**, **arithmetic**) | V = −173 ≫ 1 = **−87** |
| Clamp (off) | **−87** |

The last two rows are the point. The rounding bias is suppressed entirely for ÷2, and the final
shift floors toward negative infinity: −173/2 is −86.5, and an arithmetic shift gives −87, whereas
integer division in C (which truncates toward zero) would give −86. An implementation that writes
`U / 2` instead of `U >> 1` is wrong for every odd negative `U`.

Also note `c = 255` producing a weight of exactly 256, so the interpolation returns `b` exactly.

### Example 4 — `HALF` is not a half

`a = 255`, `b = 0`, `c = Half` (selector 13, value 128), `d = 0`, op = Add, bias = 0, scale = ×1,
clamp = on.

c′ = 129. L = 255·(256−129) + 0·129 = 255·127 = 32 385. R = (32 385 + 128) ≫ 8 = 32 513 ≫ 8 =
**127**. Result **127**.

Now exchange `a` and `b`: `a = 0`, `b = 255`.

c′ = 129. L = 0·127 + 255·129 = 32 895. R = (32 895 + 128) ≫ 8 = 33 023 ≫ 8 = **128**. Result
**128**.

The same nominal "average of 0 and 255" gives 127 one way round and 128 the other. Interpolating
with `HALF` is a 127/129 split, not a 128/128 one. Any implementation that special-cases `c = Half`
into `(a + b) / 2` will be off by one on half its inputs.

---

## 9. Implementation checklist

A condensed form of the above, for checking an existing implementation:

- [ ] `a`, `b`, `c` masked with `& 255` before anything else; `d` not masked.
- [ ] Weight widened as `c + (c >> 7)`, giving 0…256 with 128 unreachable.
- [ ] Scale applied as a **left shift on the interpolation product**, before the rounding bias.
- [ ] Scale applied **separately** to `d` and to the bias term.
- [ ] Rounding bias 128 for Add, 127 for Sub, **0 for ÷2 regardless of op**.
- [ ] `≫ 8` applied after the rounding bias, not before.
- [ ] Op sign applied to the interpolation result only — never to `d`, never to the bias.
- [ ] ÷2 applied last, as an **arithmetic** right shift on a possibly-negative value.
- [ ] Clamp 0…255 when the clamp bit is set; clamp (not truncate) to −1024…+1023 when it is clear.
- [ ] Konstant fraction for 1/2 is 128, and the sequence is 255/223/191/159/128/96/64/32.
- [ ] Konstant alpha selectors 8…15 all read 0; konstant colour selectors only 8…11 do.
- [ ] Swap tables applied to texture and rasterised colour before the input mux, selected by bits
      3:0 of the **alpha** combiner register, and affecting the colour combiner too.
- [ ] Alpha compare mode fetches its `a` and `b` operands from the **colour** combiner's selectors.
- [ ] The pixel handed to the PE is `prev` after the last stage, and it narrows to 8 bits there.
