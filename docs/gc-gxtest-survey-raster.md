# gxtest survey: `rasterization.cpp` and `clipping.cpp`

A read-only survey of two GPL test programs from `dolphin-emu/hwtests`, checked out at
`roms/gc/hwtests/gxtest/`. Nothing here is copied from them; this is a description of
what they do and what Flipper has to do for them to report a real verdict.

Files read:

- `roms/gc/hwtests/gxtest/rasterization.cpp`
- `roms/gc/hwtests/gxtest/clipping.cpp`
- `roms/gc/hwtests/gxtest/util.cpp`, `util.h`
- `roms/gc/hwtests/gxtest/cgx.cpp`, `cgx.h`, `cgx_defaults.h`
- supporting register definitions in `roms/gc/hwtests/gxtest/BPMemory.h`, `XFMemory.h`

Current backend state for reference: `runtime-gc/src/gc_gx.c` parses the FIFO and
`runtime-gc/include/gc_gx_draw.h` declares the transform stage and primitive assembly.
There is no rasteriser, no EFB, no TEV and no copy pipeline yet, which is what these two
programs need.

---

## 0. The shared harness

### 0.1 `GXTest::Init()` — inherited by every gxtest program

`util.cpp:31`. In the non-debug build (the one we run) it does, in order:

| Step | Effect |
| --- | --- |
| `CGX_Init()` | `GX_Init` with a 256 KB FIFO at a 32-byte aligned buffer; registers the PE-finish IRQ (`IRQ_PI_PEFINISH`) and writes `0x0F` to PE register at `0xCC001000 + 5*2` to unmask/ack PE interrupts. **Every libogc `GX_Init` default register write goes through the FIFO here** — that is the only source of the state the tests never write themselves. |
| `GX_SetCopyClear({0, 0x27, 0, 0xff}, 0x00ffffff)` | Copy-clear colour R=0, G=0x27, B=0, A=0xff; clear Z = 0x00FFFFFF. Never actually used: no test in these two programs sets the copy `clear` bit. |
| `GX_SetViewport(0, 0, 640, 528, 0, 1)` | Full EFB viewport. Both programs overwrite this immediately. |
| `GX_SetScissor(0, 0, 640, 528)` | **Not overwritten by either program.** This is the only thing stopping the deliberately oversized "clear" quads (which extend to the guard-band limit) from writing outside the EFB. |
| `test_buffer = memalign(32, 640*528*4)` | 1,351,680 bytes, 32-byte aligned (required: the EFB-copy destination register holds `phys_addr >> 5`). |
| `GX_SetTexCopySrc(0,0,100,100)`, `GX_SetTexCopyDst(100,100,GX_TF_RGBA8,false)` | Primes libogc's cached copy state. Irrelevant in practice — `CGX_DoEfbCopyTex` writes all five copy BP registers itself every time. |
| Vertex format | `GX_ClearVtxDesc`; `GX_VA_POS = GX_DIRECT`, `GX_VA_CLR0 = GX_DIRECT`. VTXFMT0: position `GX_POS_XYZ`/`GX_F32`/frac 0, colour 0 `GX_CLR_RGBA`/`GX_RGBA8`/frac 0. So a vertex is **16 bytes**: three f32 followed by one u32 RGBA. |
| `GX_LoadPosMtxImm(identity, GX_PNMTX0)` | Position matrix 0 = identity. |
| `CGX_LoadProjectionMatrixOrthographic` | `m00 = 1`, `m11 = 1`, `m22 = -1`, everything else 0, orthographic flag. Loads XF `0x1020..0x1026` as `(m00, m03, m11, m13, m22, m23, type=1)` = `(1, 0, 1, 0, -1, 0, 1)`. So **x' = x, y' = y, z' = −z, w = 1**. The negation matters: every quad these tests draw at `z = 1.0` ends up with clip-space `z = −1 = −w`, i.e. exactly on the far plane. |
| `GX_SetNumChans(1)`, `GX_SetNumTexGens(0)` | One colour channel, no texgens. |
| `GX_SetTevOrder(stage0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0)` and `GX_SetTevOp(stage0, GX_PASSCLR)` | One TEV stage passing the rasterised colour through: colour combiner `d = RasColor`, alpha `d = RasAlpha`, `a = b = c = Zero`, `op = Add`, `bias = Zero`, `scale = 1`, `clamp = false`, `dest = Prev`. |
| A dummy white quad | Four vertices at NDC `(±1, ±1, z=1)`, colour `0xFFFFFFFF`, drawn with `GX_Begin(GX_QUADS, GX_VTXFMT0, 4)` purely so libogc flushes its cached state into the FIFO. The emulator must tolerate it; nothing reads the result. |
| `PEControl` (BP `0x43`) | `pixel_format = RGBA6_Z24 (1)`, `zformat = ZLINEAR (0)`, `early_ztest = false`. Register value `0x43000001`. |

Two consequences worth pinning:

- **`rasterization.cpp` never changes the pixel format**, so it runs against an
  **RGBA6_Z24** EFB. Its expected values are only 0 and 255, which survive the
  8→6→8 bit round trip exactly (6-bit 63 expands to 255, 0 to 0), so this does not
  change any expectation — but the EFB store/expand path must be right.
  `clipping.cpp` explicitly switches to **RGB8_Z24**.
- **`rasterization.cpp` never writes `GenMode`, `ZMode`, the alpha-compare register or
  the blend register.** Those come from whatever libogc's `GX_Init` pushed (cull mode,
  one TEV stage, Z test LEQUAL with Z writes, alpha compare always, blend off,
  dither). The emulator does not need to *know* these defaults — they arrive over the
  FIFO — but it does need to implement the registers, and the winding of
  `GXTest::Quad` must survive `GX_Init`'s cull mode. If in doubt, log the BP writes
  `GX_Init` emits; the FIFO reader already sees them all.

### 0.2 `GXTest::Quad`

`util.cpp:172`. Corners default to NDC `(∓1, ±1, z = 1.0)`. `Draw()` re-writes the CP
vertex descriptor and VAT every time (`CP 0x50`, `0x60`, `0x70`, `0x80`, `0x90`:
position XYZ/F32 direct, colour 0 RGBA8 direct when a colour was set, `ByteDequant = 1`),
re-loads the same orthographic projection, then emits the raw draw command by hand:
`0x80` (GX_QUADS), `u16` count 4, then four vertices in the order
**top-left, top-right, bottom-right, bottom-left**.

`ColorRGBA(r,g,b,a)` packs to `(r<<24)|(g<<16)|(b<<8)|a`, i.e. RGBA8 as the VAT expects.

### 0.3 `CGX_SetViewport(origin_x, origin_y, width, height, near, far)`

`cgx.cpp:86`. Writes six XF registers at `0x101a`:

```
scale  = ( width*0.5, -height*0.5, (far-near)*16777215.0 )
offset = ( 342.0 + origin_x + width*0.5,
           342.0 + origin_y + height*0.5,
           far * 16777215.0 )
```

The `342.0` is the hardware's internal screen origin; the scissor-offset register
(BP `0x59`, set by `GX_Init`/`GX_SetScissor` to `342/2 = 171` in both fields)
subtracts it again, so **EFB x = 342 + origin_x + width*0.5 + ndc_x * width*0.5 − 342**.

With `near = 0, far = 1` (every call in both programs): `scale_z = offset_z = 16777215`,
so screen depth `= 16777215 * (ndc_z + 1)`. Combined with the `m22 = −1` projection,
a vertex at input `z = 1.0` lands at **screen depth 0** — the near end of the range.
Everything these tests draw normally is at depth 0.

**The addition `342.0f + origin_x` is done in float32 and this is load-bearing.**
See §1.5.

### 0.4 `GXTest::GetTevOutput`

Declared in `util.h`, implemented at `util.cpp:316`. It reconstructs all 11 TEV output
bits via two extra render passes. **Neither of these two programs uses it** — it is for
`tev.cpp` / `lighting.cpp`. Ignore it for this work item.

---

## 1. `rasterization.cpp`

One test function, `CoordinatePrecisionTest()`, three loops, **519 subtests total**.

Common per-loop setup (`rasterization.cpp:20-39`):

- BP `0x28` (`TwoTevStageOrders` index 0): texmap NULL, texcoord NULL, `enable0 = false`,
  `colorchan0 = Color0`.
- XF `0x1009` = 1 (one colour channel).
- XF `0x100e` and `0x1010` (colour channel 0 and alpha channel 0): `LitChannel` with
  `matsource = 1` (from vertex), `enablelighting = 0`, no lights. **No lighting.**
- BP `0xC1` (alpha combiner stage 0): `d = RasAlpha`, rest zero.
- BP `0xC0` (colour combiner stage 0): `d = RasColor`, rest zero.

So the pipeline is: vertex colour → channel 0 unlit → TEV stage 0 pass-through → EFB.

### 1.1 Loop 1 — where the left edge of a primitive starts covering a pixel (2 subtests)

`rasterization.cpp:43`. `xpos` sweeps by `nextafterf` from `0.583328247070f` to
`0.583328306675f` inclusive. Those two literals are exactly `9786624 · 2⁻²⁴` and
`9786625 · 2⁻²⁴` — adjacent floats — so **the loop runs exactly twice**.

Per iteration:

1. `CGX_SetViewport(xpos, 0, 100, 100, 0, 1)` → `scale = (50, −50, 16777215)`,
   `offset = (342+xpos+50, 392, 16777215)`. Viewport covers EFB x ≈ `xpos .. 100+xpos`,
   y `0 .. 100`.
2. Clear quad A: NDC `(±2, ±2)`, z = 1, colour `(0,0,0,255)`. Spans the full guard band
   (EFB x ≈ −100..200), relying on the 640×528 scissor to trim it.
3. Clear quad B: default quad (NDC ±1), colour `(0,255,0,255)`.
4. Test quad: default quad with top-left and bottom-left x moved to NDC `0`, colour
   `(255,0,255,255)`. Its left edge lands at EFB x = `offset_x − 342` ≈ 50.5833.
5. `CopyToTestBuffer(50, 0, 127, 127)` → EFB rect left 50, top 0, **width 78, height 128**.
6. `ReadTestBuffer(0, 0, 128)` → tile 0, texel 0 → **EFB pixel (50, 0)**. (The width
   argument disagrees with the copy width, but at (0,0) the address is 0 either way.)

Assertion: `result.r == 255` for the first `xpos`, `== 0` for the second.

**What it pins down.** `offset_x` is computed as `(342.0f + xpos) + 50.0f`:

| `xpos` | `342.0f + xpos` | `offset_x` | left edge, EFB x |
| --- | --- | --- | --- |
| `9786624·2⁻²⁴` | 342.58331298828125 | 392.58331298828125 | 50.58331298828125 |
| `9786625·2⁻²⁴` | 342.583343505859375 | 392.583343505859375 | 50.583343505859375 |

Those two literals were chosen precisely because they straddle a float rounding boundary
at the 342 scale (the exact sums are `…770.5` and `…770.502` in units of 2⁻¹⁵). A pass
therefore means the rasteriser's horizontal sample point for pixel column 50 satisfies

```
0.58331298828125  ≤  sample_x  <  0.583343505859375
```

i.e. it is within about 3·10⁻⁵ of a pixel of **7/12 = 0.58333…**.

### 1.2 Loop 2 — the exact subsample location (515 subtests)

`rasterization.cpp:83`. `xpos` sweeps from `0.583297669888f` (= `9786111·2⁻²⁴`) to
`0.583328306675` (= `9786625·2⁻²⁴`), one float ULP at a time: **515 iterations**.

Per iteration:

1. `CGX_SetViewport(0, 0, 100, 100, 0, 1)` and the same two clear quads (black over the
   guard band, green over the viewport).
2. The viewport registers are then written **by hand** (XF `0x101a`, six words) to get a
   viewport narrower than floating point could express through `CGX_SetViewport`:

   ```
   scale  = ( 2.0e-5f, -50.0f, 16777215.0f )
   offset = ( 342.0f + xpos + 2.0e-5f, 392.0f, 16777215.0f )
   ```

   Note the x offset adds the *full* width, not half, so the viewport spans screen x from
   `offset_x − 2e-5` to `offset_x + 2e-5` — **4·10⁻⁵ of a pixel wide**, sitting at
   EFB x ≈ 0.5833.
3. A default full-viewport magenta quad is drawn; it therefore covers only that sliver.
4. `CopyToTestBuffer(0, 0, 127, 127)` → 128×128 from EFB (0,0).
5. `ReadTestBuffer(0, 0, 128)` → **EFB pixel (0, 0)**.

Assertion: `result.r == 0` for the first and last `xpos`, `255` for the 513 in between.

**What it pins down.** `342.0f + xpos + 2.0e-5f` rounds to exactly three distinct floats
across the 515-iteration sweep, and they line up one-to-one with the three expectation
groups:

| iterations | `offset_x` | sliver, EFB x | expected |
| --- | --- | --- | --- |
| `n = 9786111` (1) | 342.58331298828125 | 0.58329299 … 0.58333299 | 0 (miss) |
| `n = 9786112 … 9786624` (513) | 342.583343505859375 | 0.58332351 … 0.58336351 | 255 (hit) |
| `n = 9786625` (1) | 342.5833740234375 | 0.58335402 … 0.58339402 | 0 (miss) |

Passing all 515 therefore requires the horizontal sample point to lie in

```
0.583332988…  <  sample_x  <  0.583354023…
```

— which, combined with loop 1, brackets it to
**`sample_x ∈ (0.5833330, 0.5833435)`**, a window ~1·10⁻⁵ of a pixel wide, just barely
above 7/12. This is the source of the file's comment that the subsample is "close to (but
somewhat off)" 7/12.

The vertical geometry is unconstrained here: the sliver spans EFB y 0..100, so any
positive `sample_y` works.

### 1.3 Loop 3 — the guard band boundary (2 subtests)

`rasterization.cpp:126`. `CGX_SetViewport(100, 100, 100, 100, 0, 1)` →
`scale = (50, −50, …)`, `offset = (492, 492, …)`. Viewport = EFB x 100..200, y 100..200;
guard band (NDC ±2) = EFB x 50..300 … y 50..300 clipped by the scissor.

`xpos` takes two values: `-2.0f` and `nextafterf(-2.0f, -1.0f)`.

Per iteration: black clear quad over NDC ±2, then a magenta quad with left edge at NDC
`xpos` and right edge at `xpos + 1.0`, spanning y NDC ±1. At `xpos = −2` that is
EFB x 50..100, y 100..200.

- `CopyToTestBuffer(50, 100, 127, 127)` → left 50, top 100, **width 78, height 28**.
- `ReadTestBuffer(10, 10, 128)` — **the width argument (128) does not match the copy
  width (78)**, so this does *not* read EFB (60, 110). Byte offset 4244 under the
  78-wide tiling is tile 66, texel 10 → tile row 3, tile column 6, in-tile (2, 2) →
  s = 26, t = 14 → **EFB pixel (76, 114)**. That is still inside the magenta quad, so
  the test works anyway, but an emulator must reproduce the 78-wide tiling for the
  address to land there.

Assertion: `expectation = (xpos >= -2.0) ? 255 : 0`. **Note that
`nextafterf(-2.0f, -1.0f)` moves *toward zero*, so it is `−1.99999988`, which is
`>= -2.0`. Both subtests therefore expect 255.** The "clipped" branch of the ternary is
dead code; this loop only asserts that the guard band includes NDC −2.0 inclusively and
does not actually exercise a clipped primitive. Worth knowing before treating a pass here
as evidence the guard band is right.

### 1.4 What `rasterization.cpp` needs from the GPU

| Area | Needed |
| --- | --- |
| CP | `0x50`/`0x60` vertex descriptor, `0x70`/`0x80`/`0x90` VAT 0; direct f32 XYZ + RGBA8 colour 0; the raw `0x80` quad command with a u16 count. |
| XF | position matrix memory (PNMTX0 = identity); projection `0x1020..0x1026` (orthographic); viewport `0x101a..0x101f`; `0x1009` number of colour channels; `0x100e`/`0x1010` channel control with `matsource = vertex`, lighting off. Does **not** write `0x1005` (clip disable) — clipping stays enabled. |
| BP | `0x00` GenMode (inherited from `GX_Init`, never written here); `0x20`/`0x21`/`0x59` scissor and scissor offset; `0x28` TevOrder 0; `0x40` ZMode (inherited: LEQUAL, test on, write on); `0x43` PEControl (RGBA6_Z24 from `Init`); `0xC0`/`0xC1` TEV stage 0 combiners; copy registers `0x49`, `0x4A`, `0x4B`, `0x4D`, `0x52`; `0x45` draw-done token. |
| Lighting | **No.** Unlit channel, colour straight from the vertex. |
| Texturing | **No.** Zero texgens, TEV stage 0 has texmap/texcoord NULL and `enable0 = false`. |
| TEV | Only pass-through: `d = RasColor` / `d = RasAlpha`, `a = b = c = Zero`, add, bias 0, scale 1, no clamp, dest Prev. No konst, no TEV registers, no indirect stages. |
| Z compare | Inherited from `GX_Init` (LEQUAL, test on, write on). All geometry is at screen depth 0, so a correct LEQUAL never rejects — but the register must exist and must not reject. The EFB is never cleared; any initial Z ≥ 0 works. |
| Alpha compare | Inherited default: always pass. |
| Blending | Inherited default: off (`ONE`/`ZERO`), colour update on. Dither is on by default; harmless because the only values written are 0 and 255. |
| Scissor | **Yes, essential.** The 640×528 scissor is the only thing clipping the deliberately guard-band-sized clear quads. |
| Guard band | **Yes**, at NDC ±2, inclusive at exactly −2.0 (loop 3, and the clear quads in loops 1 and 2 depend on the guard band not rejecting them). |

### 1.5 Minimum viable, then passing

**To report real results** (each `DO_TEST` produces a genuine verdict rather than every
subtest failing identically):

1. Transform: position matrix → orthographic projection → perspective divide (w = 1
   throughout) → viewport scale/offset → subtract 342 via the scissor offset register.
2. Quad → two triangles, with the `GXTest::Quad` corner order.
3. A triangle rasteriser over a 640×528 EFB with *some* sample point and fill rule.
4. Flat colour is enough (every quad here is single-coloured), through a single TEV
   stage implementing `d ∈ {RasColor, Zero}`.
5. EFB writes in RGBA6_Z24, expanded back to 8 bits on copy.
6. Scissor test against BP `0x20`/`0x21` with the `0x59` offset.
7. The EFB→texture copy of §3, RGBA8, correct tiling.
8. The draw-done / PE-finish interrupt so `CGX_WaitForGpuToFinish` returns.
9. **The clipper must treat clip-space `z == −w` as inside.** Every quad in this file
   sits exactly on the far plane (input z = 1.0, `m22 = −1`). A strict inequality there
   makes all 519 subtests fail with nothing drawn at all.

**To pass**, additionally:

- The horizontal sample point must be correct to roughly **1·10⁻⁵ of a pixel**
  (§1.2). The coverage test cannot snap to a coarse 1/12 or 1/16 sub-pixel grid — a
  4·10⁻⁵-pixel-wide viewport must produce 513 consecutive hits, which a coarse grid
  cannot do.
- The viewport offset must be consumed exactly as the float32 the FIFO delivered; no
  promotion to double before the edge comparison, and no re-derivation of the offset
  from other quantities.
- Guard band inclusive at NDC −2.0.

---

## 2. `clipping.cpp`

One test function, `ClipTest()`, a 16-iteration loop, **16 subtests**.

Setup before the loop (`clipping.cpp:20-44`) is the same TEV/channel setup as
`rasterization.cpp` (TevOrder 0 all-NULL, one colour channel, unlit from vertex), plus:

- BP `0xC1` alpha combiner stage 0 = **all zero** (so TEV alpha output is 0 — irrelevant,
  the format is RGB8).
- BP `0x00` GenMode: `numtexgens = 0`, `numcolchans = 1`, `numtevstages = 0` (one stage),
  `cullmode = None`, `numindstages = 0`, `zfreeze = false`.
- BP `0x43` PEControl: `pixel_format = RGB8_Z24 (0)`, `zformat = ZLINEAR`,
  `early_ztest = false`. Register value `0x43000000`.

Per step, before the switch:

- BP `0x40` ZMode reset to the default: **test disabled, func Always, update disabled**.
  So `clipping.cpp` does no depth testing at all; everything it asserts about depth is
  clipping, not Z compare.
- Three "clear" passes, each with its own viewport and `cc.d = RasColor`:

  | viewport | scale / offset | EFB rect | colour |
  | --- | --- | --- | --- |
  | `(0, 0, 201, 50)` | `(100.5, −25)` / `(442.5, 367)` | x 0..201, y 0..50 | black `(0,0,0,255)` |
  | `(75, 0, 100, 50)` | `(50, −25)` / `(467, 367)` | x 75..175, y 0..50 | dark green `(0,0x7f,0,255)` |
  | `(100, 0, 50, 50)` | `(25, −25)` / `(467, 367)` | x 100..150, y 0..50 | bright green `(0,0xff,0,255)` |

  The middle one is exactly the guard-band extent of the third (NDC ±2 of a 50-wide
  viewport at x 100 is EFB x 75..175), so the three bands let a human see at a glance
  whether a primitive landed inside the viewport, inside the guard band, or outside.

- The test viewport is then `(100, 0, 50, 50)` again, so for the test quad:

  ```
  EFB x = 125 + 25 * ndc_x        EFB y = 25 - 25 * ndc_y
  ```

  Viewport = x 100..150, y 0..50. Guard band = x 75..175, y −25..75.

- `cc.d = TevColorArg::Color0`, and TEV colour register c0 (BP `0xE2`/`0xE3`) is set to
  red = 0xFF, green/blue 0. So the test quad is drawn **red from a TEV register**, not
  from its vertex colour; the check is `result.r == 0xff` (drawn) or `0x00` (hidden)
  against the black/green clears, all of which have red = 0.
- XF `0x1005` = 0 (clipping enabled), overridden in some steps.

Readback in every step: `CopyToTestBuffer(0, 0, 199, 49)` → EFB rect **200 × 50** from
(0,0), then `ReadTestBuffer(test_x, test_y, 200)` — width matches the copy here, so the
addressing is correct.

### 2.1 The 16 steps

`test_x` defaults to 125 and `test_y` to 25 — the centre of the viewport.

| # | Geometry (NDC) | EFB footprint | read at | expects | Comment |
| --- | --- | --- | --- | --- | --- |
| 0 | default quad, x ±1, y ±1, z = 1 | x 100..150, y 0..50 | (125,25) | drawn | Baseline. Consistent. |
| 1 | left edge to x = −1.8 (outside viewport, inside guard band) | x 80..150 | (75,25) | drawn | **Read point is outside the drawn quad** (80 > 75). The file's own TODO admits the read point is provisional ("debug readback issues first"). |
| 2 | left edge to x = −2.5 (outside the guard band) | clipped to x 75..150 | (51,25) | drawn | **Read point is outside the guard band**; the file's TODO says so explicitly. |
| 3 | x −1.5..1.5, bottom-right y set to `+1.0` (degenerate) | triangle (87.5,0)–(162.5,0)–(87.5,50) | (80,25) | drawn | Read point outside the triangle. TODO: "Move closer to actual viewport". |
| 4 | x ±2.5, same degenerate bottom-right | clipped to guard band x 75..175 | (51,25) | drawn | Read point outside the guard band. |
| 5 | TL(−1.8,1), BR(−1.2,1), BL(−1.8,−1), then TR overwritten to (1.5,1) | one degenerate triangle plus (80,0)–(95,0)–(80,50) | (125,25) | hidden | Consistent — but only accidentally: the final `VertexTopRight` overwrite contradicts the comment ("all on the same side"). |
| 6 | top-left x to −4.0 | clipped at guard band x 75 | (51,1) | drawn | Read point outside the guard band. Marked "Currently broken" in the file. |
| 7 | all z = 1.1 → clip z = −1.1 < −w; XF `0x1005` = 0 | — | (125,25) | hidden | Sound. |
| 8 | same, XF `0x1005` = 1 | — | (125,25) | hidden | Sound. See §2.2. |
| 9 | all z = −0.00001 → clip z = +1e-5 > 0; `0x1005` = 0 | — | (125,25) | hidden | Sound. |
| 10 | same, `0x1005` = 1 | — | (125,25) | hidden | Sound. |
| 11 | all z = 1.0000001 → clip z + w = −1.19e-7 by IEEE; `0x1005` = 0 | x 100..150, y 0..50 | (125,25) | drawn | Sound, and the hardest one. See §2.3. |
| 12 | same, `0x1005` = 1 | same | (125,25) | drawn | Sound. |
| 13 | top-left z = 1.5 (clip z = −1.5), others exactly on the far plane; `0x1005` = 0 | nothing survives clipping | (125,25) | hidden | Sound. |
| 14 | same, `0x1005` = 1 | whole quad drawn | (125,25) | drawn | Sound. |
| 15 | three vertices at z = 65537 (clip z = −65537), bottom-right at z = 1; `0x1005` = 1 | whole quad drawn | (125,25) | drawn | Sound. |

**Five of the sixteen steps (1, 2, 3, 4, 6) assert red at a pixel the geometry does not
cover**, and each carries a TODO in the source saying the read coordinate is provisional.
Do not treat those five as a correctness target for the emulator until the geometry is
re-derived; they read the guard-band or clear bands, not the test quad. The eleven
remaining steps (0, 5, 7–15) are internally consistent and are the real signal.

### 2.2 The clipping model the sound steps imply

`XF 0x1005` is the clip-disable register. Only **bit 0** (disable clipping detection) is
ever written by this test; **bit 1 (disable trivial rejection) is never set**, so trivial
rejection stays on throughout. That resolves what would otherwise look contradictory:

- Steps 7–10: the primitive is *entirely* beyond a depth plane → **trivially rejected
  regardless of bit 0** → hidden in both the enabled and disabled variants.
- Steps 13/14: the primitive *straddles* a depth plane → trivial rejection does not
  apply → bit 0 decides. Clipping on (13): nothing survives. Clipping off (14): the whole
  primitive is drawn, out-of-range depth and all.
- Step 15: only three of four vertices are out of range (the fourth sits exactly at
  z = −w), so no trivial rejection; clipping is off, so the whole quad is drawn even
  with an interpolated depth of roughly −65537·2²⁴.

The valid clip-space depth range implied by steps 7–10 is **−w ≤ z ≤ 0** (not −w..+w):
step 9 rejects `z = +1e-5`, step 7 rejects `z = −1.1` with `w = 1`. The default quad's
`z = −w` must be **inside**, inclusively.

### 2.3 Step 11/12 — the non-IEEE clipper

Input z = 1.0000001 → clip-space z = −1.0000001, w = 1. In IEEE single precision
`z + w = −0.00000011920928955078125`, which is negative, so a strict IEEE clipper rejects
the primitive. The GameCube GPU's clipper does not use strict IEEE arithmetic and treats
that sum as zero, so the primitive survives and the test expects it drawn. The file
carries the comment that this "doesn't actually work, yet" — i.e. it was a known failure
against Dolphin at the time it was written. It requires modelling the GPU's reduced
mantissa in the clip-plane evaluation, not just clamping.

### 2.4 What `clipping.cpp` needs from the GPU

Same as `rasterization.cpp` for CP, XF viewport/projection/channels, scissor and the copy
path, plus:

| Area | Needed |
| --- | --- |
| XF `0x1005` | Clip-disable register, bit 0 only, re-written per step. |
| BP `0x00` GenMode | Written explicitly: cull None, one TEV stage. |
| BP `0x40` ZMode | Written explicitly per step: **test off, Always, no update**. No depth testing; a Z-compare implementation is not required for this program, but the register must be honoured (an emulator that ignores `testenable` and tests anyway will fail steps 14 and 15, whose depths are wildly out of range). |
| BP `0x43` PEControl | RGB8_Z24. Alpha in the copy is forced to 0xFF; the test never checks alpha. |
| BP `0xE2`/`0xE3` | TEV colour register c0 = (255, 0, 0). Needed: the test quad's colour comes from here, not from the vertex. |
| TEV | `d = Color0` for the test quad, `d = RasColor` for the clears. Still one stage, no texturing, no lighting, no konst. |
| Guard band | NDC ±2 in x, exercised by steps 1–6. |
| Depth clipping | Range −w ≤ z ≤ 0, inclusive at both ends, with trivial rejection independent of the clip-disable bit. |

### 2.5 Minimum viable, then passing

**To report real results**: the same floor as §1.5 items 1–8, plus honouring
`ZMode.testenable = false`, TEV colour registers, and the `z == −w` inclusive rule
(item 9 — which for this program means *all sixteen* steps, since every clear quad and
the default test quad sit on the far plane).

With that floor and no clipping at all (draw everything, scissor only), the eleven sound
steps would report: 0 pass, 5 hidden-expected fail, and 7/8/9/10/13 fail. That is a
useful first signal.

**To pass the eleven sound steps**, additionally:

1. Trivial rejection: reject a primitive when all vertices are outside the same plane,
   unconditionally (steps 7–10).
2. Clip-disable bit 0 gating clipping of straddling primitives (13 vs 14, 15).
3. Depth range −w ≤ z ≤ 0 inclusive.
4. Guard band clipping at NDC ±2 in x and y (needed for the clear quads even in the
   sound steps).
5. Reduced-precision clip-plane arithmetic for steps 11 and 12.

Steps 1, 2, 3, 4 and 6 need the read coordinates fixed in the test source before they can
be made to pass; they are not an emulator bug.

---

## 3. Result readback — the memory layout that must be exact

This is the part the emulator has to reproduce byte for byte.

### 3.1 `GXTest::CopyToTestBuffer(left, top, right, bottom, params)`

`util.cpp:307`:

1. `memset(test_buffer, 0, 640*528*4)` — **1,351,680 bytes zeroed before every copy**.
   (519 + 16 copies across the two programs; worth knowing for runtime.)
2. Calls `CGX_DoEfbCopyTex(left, top, right-left+1, bottom-top+1, test_buffer, params)`.
   Both programs pass the default `EFBCopyParams{}`.

### 3.2 `CGX_DoEfbCopyTex(left, top, width, height, dest, params)`

`cgx.cpp:133`. Asserts all four of left/top/width/height ≤ 1023, then writes exactly five
BP registers and one cache operation:

| BP reg | Value |
| --- | --- |
| `0x49` `BPMEM_EFB_TL` | `X10Y10`: `x = left` (bits 0–9), `y = top` (bits 10–19) |
| `0x4A` `BPMEM_EFB_WH` | `X10Y10`: `x = width − 1`, `y = height − 1` |
| `0x4D` `BPMEM_MIPMAP_STRIDE` | `((width + 3) >> 2) * 2` — destination stride in 32-byte units, i.e. `tiles_per_row × 64 / 32`. Hardcoded for RGBA8 (the source comments say so). |
| `0x4B` `BPMEM_EFB_ADDR` | `MEM_VIRTUAL_TO_PHYSICAL(dest) >> 5` — hence the 32-byte alignment of `test_buffer`. |
| `0x52` `BPMEM_TRIGGER_EFB_COPY` | `UPE_Copy`, see below |

With the default `EFBCopyParams`, the trigger register's payload is:

| field | bits | value |
| --- | --- | --- |
| `clamp_top` | 0 | 1 |
| `clamp_bottom` | 1 | 1 |
| `unknown_bit` | 2 | 0 |
| `target_pixel_format` | 3–6 | `0xC` — `SetRealFormat(EFBCopyFormat::RGBA8 = 0x6)` computes `((6&7)<<1) | ((6&8)>>3) = 0xC` |
| `gamma` | 7–8 | 0 (`Gamma1_0`) |
| `half_scale` | 9 | 0 |
| `scale_invert` | 10 | 0 |
| `clear` | 11 | **0 — the copy does not clear the EFB.** Both programs repaint by drawing quads. |
| `frame_to_field` | 12–13 | 0 (Progressive) |
| `copy_to_xfb` | 14 | 0 — texture copy, not XFB |
| `intensity_fmt` | 15 | 0 |
| `auto_conv` | 16 | 0 |

So the written word is **`0x52000063`** for every copy in both programs.

`clamp_top` and `clamp_bottom` both set means the vertical copy filter clamps at both
edges; with the default copy-filter coefficients `GX_Init` loaded, the result is an
unfiltered 1:1 copy. (`copyfilter.cpp` is the test that exercises the filter; these two
do not.)

Finally `DCInvalidateRange(dest, GX_GetTexBufferSize(width, height, GX_TF_RGBA8, GX_FALSE, 1))`
— i.e. `ceil(width/4) * ceil(height/4) * 64` bytes. The emulator must land the copy in
main RAM before the CPU's post-invalidate read; the synchronisation point is
`CGX_WaitForGpuToFinish` (`cgx.cpp:230`), which writes BP `0x45` = **`0x45000002`**
(draw-done), pushes 32 bytes of zero through the write-gather pipe
(`CGX_ForcePipelineFlush`), and blocks until the PE-finish interrupt fires.

### 3.3 `GXTest::ReadTestBuffer(s, t, width)` — the tiling

`util.cpp:151`. The addressing is, in full:

```
sBlk      = s >> 2
tBlk      = t >> 2
widthBlks = (width + 3) >> 2
base      = (tBlk * widthBlks + sBlk) << 5      // tile index × 32
blkS      = s & 3
blkT      = t & 3
blkOff    = (blkT << 2) + blkS                  // texel index 0..15 within the tile
offset    = (base + blkOff) << 1                // bytes
addr      = (u8*)test_buffer + offset

r = addr[1]      g = addr[32]      b = addr[33]      a = addr[0]
```

In plain terms — the standard GX `TF_RGBA8` layout:

- Texels are grouped into **4 × 4 tiles**, **64 bytes each**.
- Tiles are stored **row-major**: `tile_index = (t/4) * ceil(width/4) + (s/4)`, and the
  tile begins at byte `tile_index * 64`.
- Each tile is **two 32-byte halves**. The first half holds **alpha/red** interleaved,
  the second half holds **green/blue** interleaved.
- Within a tile, texel index `i = (t%4)*4 + (s%4)`, and:

  | channel | byte address |
  | --- | --- |
  | A | `tile_base + 2*i` |
  | R | `tile_base + 2*i + 1` |
  | G | `tile_base + 32 + 2*i` |
  | B | `tile_base + 32 + 2*i + 1` |

- Total copy size = `ceil(width/4) * ceil(height/4) * 64` bytes.

Concrete sizes in these two programs:

| call site | copy rect | tiles | bytes written |
| --- | --- | --- | --- |
| `rasterization` loop 1, loop 3 | 78 × 128 / 78 × 28 | 20 × 32 / 20 × 7 | 40,960 / 8,960 |
| `rasterization` loop 2 | 128 × 128 | 32 × 32 | 65,536 |
| `clipping`, every step | 200 × 50 | 50 × 13 | 41,600 |

**Only `.r` is ever checked** by either program. `.a` comes from the byte the hardware
writes for alpha: 0xFF in RGB8_Z24 (`clipping`), the 6-bit EFB alpha expanded to 8 bits
in RGBA6_Z24 (`rasterization`). Neither is asserted on.

**Width-argument mismatches.** `ReadTestBuffer`'s `width` must be the same width the copy
used, because `widthBlks` sets the tile-row stride. Two call sites in
`rasterization.cpp` pass 128 after a 78-wide copy:

- loop 1, `ReadTestBuffer(0, 0, 128)` — offset 0 either way, so it correctly reads
  EFB (50, 0).
- loop 3, `ReadTestBuffer(10, 10, 128)` — offset 4244, which under the real 78-wide
  tiling is tile 66, texel 10 → **EFB (76, 114)**, not the intended (60, 110). Still
  inside the drawn quad, so the subtest works; but the emulator must produce the
  78-wide tiling for that byte to hold the right pixel. Getting the tiling "close
  enough" is not enough here.

### 3.4 What this means for the emulator

The copy path needs:

1. The five BP registers above decoded, including the `X10Y10` bit packing and the
   `target_pixel_format` nibble's cycling-right-shift encoding (`0xC` ⇒ RGBA8).
2. EFB source rect `(left, top)` size `(width, height)` read out of a 640 × 528 EFB in
   whichever pixel format `PEControl` selects, expanded to 8 bits per channel
   (RGBA6 → RGBA8 by bit replication; RGB8 → alpha forced 0xFF).
3. Written to `phys_addr` in the 4×4-tile AR/GB layout above, with the row stride the
   `0x4D` register specifies (which for RGBA8 is `ceil(width/4) * 64` bytes).
4. Landing in main memory (uncached) before the PE-finish interrupt is raised in response
   to the BP `0x45` = `0x45000002` draw-done token.
5. No EFB clear (the `clear` bit is 0).

---

## 4. Summary of the two hardest requirements

**`rasterization.cpp` — sub-pixel sample position, to ~10⁻⁵ of a pixel.** Loop 2 sweeps
the viewport x-offset one float ULP at a time across a viewport four hundred-thousandths
of a pixel wide, and asserts a clean run of 513 hits bracketed by one miss on each side.
That works out to pinning the horizontal sample point to `sample_x ∈ (0.5833330,
0.5833435)` — just above 7/12. A rasteriser that snaps coverage to a 1/12 or 1/16
sub-pixel grid cannot produce that result at all; the edge/sample comparison has to be
done at the precision the hardware uses, on the exact float32 the XF register received.

**`clipping.cpp` — non-IEEE clip-plane arithmetic (steps 11/12).** A quad at input
z = 1.0000001 gives `z + w = −1.19·10⁻⁷` in strict IEEE single precision, so a
straightforward clipper rejects it; the hardware's reduced-precision arithmetic treats it
as zero and draws it. The test's own comment records this as a known failure. Everything
else in that file — the −w ≤ z ≤ 0 range, trivial rejection being independent of the
clip-disable bit, the NDC ±2 guard band — is ordinary work by comparison.

Two lower-level gotchas that will bite before either of those: **clip-space `z == −w`
must count as inside** (every quad in both programs sits exactly on the far plane, so a
strict comparison yields a blank EFB and 535 uniform failures), and **`ReadTestBuffer`'s
tiling must be reproduced literally**, including the loop-3 call whose width argument
disagrees with the copy width and therefore reads EFB (76, 114) rather than (60, 110).
