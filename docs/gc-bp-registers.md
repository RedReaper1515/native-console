# GameCube Blitting Processor (BP) Registers

## Provenance

Every fact in this document was derived by reading devkitPro/libogc's `gx.c` (zlib licence), the
GX implementation that shipped on real hardware for twenty years. Register numbers and bit
positions are read directly out of the `GX_LOAD_BP_REG(...)` call sites and the `__gx->*` shadow
registers that feed them. Where a layout is not self-evident, the shifting expression from `gx.c`
is quoted verbatim together with its line number, so the claim can be re-checked against the
source.

Dolphin's `BPMemory.h` (GPL-2.0, in `roms/gc/hwtests/gxtest/`) was **read as a cross-check only**.
No code, comment text, or wording from it has been copied. Where Dolphin independently agrees on a
register number or bit position, this document says "cross-checked" — that is a statement about a
fact, not a reuse of expression. Where Dolphin's copy of the header is trimmed and could not
confirm something, that is stated explicitly.

Anything that could not be pinned down from `gx.c` is marked **Unverified:**. A missing field is
better than a wrong one.

Line numbers refer to the copy of `gx.c` used for this analysis (5199 lines,
`scratchpad/gx.c`).

---

## 1. FIFO encoding

A BP register write is the single opcode byte `0x61` followed by one big-endian 32-bit word
(`gx.c:28-33`):

```c
#define GX_LOAD_BP_REG(x)               \
    do {                                \
        wgPipe->U8 = 0x61;              \
        asm volatile ("" ::: "memory" ); \
        wgPipe->U32 = (u32)(x);         \
        asm volatile ("" ::: "memory" ); \
    } while(0)
```

| Bits | Meaning |
|---|---|
| 31:24 | Register number (this is the "register" column throughout this document) |
| 23:0 | Payload |

libogc always builds the word by OR-ing the register number into bits 31:24, e.g.
`(_SHIFTL(0x40,24,8))` for ZMode (`gx.c:1133`). The helper macros used everywhere below are the
standard libogc ones:

* `_SHIFTL(v,s,w)` — take the low `w` bits of `v` and place them at bit `s`.
* `_SHIFTR(v,s,w)` — extract `w` bits starting at bit `s`.

So `_SHIFTL(x,8,3)` means "a 3-bit field at bits 10:8".

One more relevant macro (`gx.c:64`):

```c
#define XY(x, y)   (((y) << 10) | (x))
```

i.e. a packed pair with **x at bits 9:0 and y at bits 19:10**.

### 1.1 Register-number index

| Reg | Name | Section |
|---|---|---|
| `0x00` | genMode | [2](#2-genmode-0x00) |
| `0x01`–`0x04` | Copy filter sample patterns | [9.5](#95-copy-filter--sample-pattern-0x010x04) |
| `0x06`–`0x0E` | Indirect matrices A/B/C ×3 | [11.2](#112-indirect-matrices-0x060x0e) |
| `0x0F` | Indirect texture mask | [11.3](#113-indirect-texture-mask-0x0f) |
| `0x10`–`0x1F` | Indirect command, per TEV stage | [11.1](#111-indirect-command-0x100x1f) |
| `0x20` | Scissor top-left | [3](#3-scissor) |
| `0x21` | Scissor bottom-right | [3](#3-scissor) |
| `0x22` | Line/point width | [12.3](#123-linepoint-width-0x22) |
| `0x23`, `0x24` | Performance counter 0 select | [12.4](#124-performance-and-misc-registers) |
| `0x25`, `0x26` | Indirect texture coordinate scale | [11.4](#114-indirect-texture-coordinate-scale-0x25-0x26) |
| `0x27` | Indirect texture order | [11.5](#115-indirect-texture-order-0x27) |
| `0x28`–`0x2F` | TEV stage order (2 stages per register) | [7.3](#73-tev-stage-order-0x280x2f) |
| `0x30`+2n | SU texture S size (n = 0..7) | [8.5](#85-su-texture-size-registers-0x300x3f) |
| `0x31`+2n | SU texture T size (n = 0..7) | [8.5](#85-su-texture-size-registers-0x300x3f) |
| `0x40` | Z mode | [4.1](#41-z-mode-0x40) |
| `0x41` | Colour mode 0 / blend | [5.1](#51-colour-mode-0--blend-0x41) |
| `0x42` | Constant (destination) alpha | [6.2](#62-destination-alpha-0x42) |
| `0x43` | PE control | [4.2](#42-pe-control-0x43) |
| `0x44` | Field mask | [12.1](#121-field-mask-0x44) |
| `0x45` | Draw done | [10.2](#102-draw-done-0x45) |
| `0x46` | Bus clock 0 | [12.4](#124-performance-and-misc-registers) |
| `0x47` | PE token | [10.1](#101-pe-token-0x47-0x48) |
| `0x48` | PE token with interrupt | [10.1](#101-pe-token-0x47-0x48) |
| `0x49` | EFB copy source top-left | [9.1](#91-efb-copy-source-rectangle-0x49-0x4a) |
| `0x4A` | EFB copy source width/height | [9.1](#91-efb-copy-source-rectangle-0x49-0x4a) |
| `0x4B` | EFB copy destination address | [9.2](#92-efb-copy-destination-0x4b-0x4d) |
| `0x4D` | EFB copy destination stride | [9.2](#92-efb-copy-destination-0x4b-0x4d) |
| `0x4E` | EFB copy vertical scale | [9.3](#93-copy-vertical-scale-0x4e) |
| `0x4F` | Copy clear alpha/red | [9.4](#94-copy-clear-colour-and-z-0x4f0x51) |
| `0x50` | Copy clear green/blue | [9.4](#94-copy-clear-colour-and-z-0x4f0x51) |
| `0x51` | Copy clear Z | [9.4](#94-copy-clear-colour-and-z-0x4f0x51) |
| `0x52` | EFB copy trigger | [9.7](#97-efb-copy-trigger-0x52) |
| `0x53`, `0x54` | Copy vertical filter weights | [9.6](#96-copy-vertical-filter-0x53-0x54) |
| `0x55`, `0x56` | Clear bounding box | [12.4](#124-performance-and-misc-registers) |
| `0x58` | Revision bits | [12.4](#124-performance-and-misc-registers) |
| `0x59` | Scissor offset | [3.3](#33-scissor-offset-0x59) |
| `0x60`–`0x63` | Texture preload | [8.7](#87-texture-preload-registers-0x600x63) |
| `0x64` | TLUT source address | [8.6](#86-tlut-registers) |
| `0x65` | TLUT destination / size | [8.6](#86-tlut-registers) |
| `0x66` | Texture invalidate | [8.8](#88-texture-invalidate-0x66) |
| `0x67` | Performance counter 1 select | [12.4](#124-performance-and-misc-registers) |
| `0x68` | Field mode | [12.2](#122-field-mode-0x68) |
| `0x69` | Bus clock 1 | [12.4](#124-performance-and-misc-registers) |
| `0x80`–`0x83`, `0xA0`–`0xA3` | Texture setmode0 | [8.1](#81-texture-setmode0-0x800x83-0xa00xa3) |
| `0x84`–`0x87`, `0xA4`–`0xA7` | Texture setmode1 | [8.2](#82-texture-setmode1-0x840x87-0xa40xa7) |
| `0x88`–`0x8B`, `0xA8`–`0xAB` | Texture setimage0 | [8.3](#83-texture-setimage0-0x880x8b-0xa80xab) |
| `0x8C`–`0x8F`, `0xAC`–`0xAF` | Texture setimage1 | [8.4](#84-texture-setimage1-2-3) |
| `0x90`–`0x93`, `0xB0`–`0xB3` | Texture setimage2 | [8.4](#84-texture-setimage1-2-3) |
| `0x94`–`0x97`, `0xB4`–`0xB7` | Texture setimage3 | [8.4](#84-texture-setimage1-2-3) |
| `0x98`–`0x9B`, `0xB8`–`0xBB` | Texture TLUT binding | [8.6](#86-tlut-registers) |
| `0xC0`+2n | TEV colour combiner, stage n (n = 0..15) | [7.1](#71-tev-colour-combiner-0xc0-2n) |
| `0xC1`+2n | TEV alpha combiner, stage n (n = 0..15) | [7.2](#72-tev-alpha-combiner-0xc1-2n) |
| `0xE0`+2n | TEV colour register n, red/alpha | [7.5](#75-tev-colour-and-konstant-registers-0xe00xe7) |
| `0xE1`+2n | TEV colour register n, blue/green | [7.5](#75-tev-colour-and-konstant-registers-0xe00xe7) |
| `0xE8` | Fog range base | [13.2](#132-fog-range-adjust-0xe80xed) |
| `0xE9`–`0xED` | Fog range table K0..K9 | [13.2](#132-fog-range-adjust-0xe80xed) |
| `0xEE` | Fog parameter A | [13.1](#131-fog-parameters-0xee0xf2) |
| `0xEF` | Fog B magnitude | [13.1](#131-fog-parameters-0xee0xf2) |
| `0xF0` | Fog B exponent | [13.1](#131-fog-parameters-0xee0xf2) |
| `0xF1` | Fog C / projection / type | [13.1](#131-fog-parameters-0xee0xf2) |
| `0xF2` | Fog colour | [13.1](#131-fog-parameters-0xee0xf2) |
| `0xF3` | Alpha compare | [6.1](#61-alpha-compare-0xf3) |
| `0xF4` | Z texture bias | [12.5](#125-z-texture-0xf4-0xf5) |
| `0xF5` | Z texture type/op | [12.5](#125-z-texture-0xf4-0xf5) |
| `0xF6`–`0xFD` | TEV konstant select / swap table | [7.4](#74-tev-konstant-select--swap-table-0xf60xfd) |
| `0xFE` | BP write mask | [12.6](#126-bp-write-mask-0xfe) |

---

## 2. genMode (`0x00`)

Register number from `gx.c:1122`:
`__gx->genMode = (__gx->genMode&~0xff000000)|(_SHIFTL(0x00,24,8));`

Written by `__GX_SetGenMode()` (`gx.c:923-927`).

| Bits | Field | Evidence |
|---|---|---|
| 3:0 | Number of texture coordinate generators (texgens), 0..8 | `__gx->genMode = (__gx->genMode&~0xf)\|(nr&0xf);` — `gx.c:2939` |
| 6:4 | Number of colour channels, 0..2 | `(_SHIFTL(num,4,3))` — `gx.c:3984`; read back as `_SHIFTR(__gx->genMode,4,3)` for XF `0x1009` at `gx.c:1010` |
| 7 | **Unverified:** unused. libogc never writes it. Dolphin marks it unused. |
| 8 | **Unverified:** flat shading. libogc never writes it; Dolphin marks it unconfirmed. Treat as unknown. |
| 9 | Multisample enable. libogc sets it only for pixel format `GX_PF_RGB565_Z16` | `(_SHIFTL(ms_en,9,1))` — `gx.c:4410` |
| 13:10 | Number of TEV stages **minus one** (0 means 1 stage, 15 means 16) | `(_SHIFTL((num-1),10,4))` — `gx.c:4035`; read back as `(_SHIFTR(__gx->genMode,10,4))+1` at `gx.c:874` |
| 15:14 | Cull mode | `(_SHIFTL(cm2hw[mode],14,2))` — `gx.c:3933` |
| 18:16 | Number of indirect texture stages, 0..4 | `(_SHIFTL(nstages,16,3))` — `gx.c:4103`; read back as `_SHIFTR(__gx->genMode,16,3)` at `gx.c:875` |
| 19 | Coplanar / Z-freeze | `(_SHIFTL(enable,19,1))` — `gx.c:3939` |
| 23:20 | **Unverified:** not written by libogc. |

### Cull mode encoding

libogc translates the API value through `static const u8 cm2hw[] = { 0, 2, 1, 3 };` (`gx.c:3931`)
where the API order is NONE, FRONT, BACK, ALL. So the **hardware** field is:

| Value | Meaning |
|---|---|
| 0 | Cull nothing |
| 1 | Cull back faces (API `GX_CULL_BACK`) |
| 2 | Cull front faces (API `GX_CULL_FRONT`) |
| 3 | Cull everything |

Note the swap: API FRONT maps to hardware 2, API BACK maps to hardware 1.

Cross-checked against Dolphin: same bit positions for all of the above.

---

## 3. Scissor

### 3.1 Scissor top-left (`0x20`)

Register number from `gx.c:1119`. Written by `GX_SetScissor()` (`gx.c:3957-3972`).

| Bits | Field | Evidence |
|---|---|---|
| 11:0 | Top edge Y, biased by +0x156 (342) | `__gx->sciTLcorner = (__gx->sciTLcorner&~0x7ff)\|(yo&0x7ff);` where `yo = yOrigin+0x156` — `gx.c:3959, 3965` |
| 23:12 | Left edge X, biased by +0x156 (342) | `(_SHIFTL(xo,12,11))` — `gx.c:3966` |

### 3.2 Scissor bottom-right (`0x21`)

Register number from `gx.c:1120`.

| Bits | Field | Evidence |
|---|---|---|
| 11:0 | Bottom edge Y, biased by +0x156, inclusive (`yo + ht - 1`) | `(__gx->sciBRcorner&~0x7ff)\|(nht&0xfff)` — `gx.c:3968` |
| 23:12 | Right edge X, biased by +0x156, inclusive (`xo + wd - 1`) | `(_SHIFTL(nwd,12,11))` — `gx.c:3969` |

**Field width caution.** The hardware fields are 12 bits each (cross-checked: Dolphin's packed
12/12 pair has y at 11:0 and x at 23:12). libogc's *masks* are narrower than its *values*:
`_SHIFTL(xo,12,11)` writes only 11 bits of X, and the clear mask `&~0x7ff` on the BR Y field
clears 11 bits while `(nht&0xfff)` supplies 12. Implement the register as **12 bits Y at 11:0,
12 bits X at 23:12**; libogc's narrower shifts are a source-side limitation, not a hardware one.

### 3.3 Scissor offset (`0x59`)

`gx.c:3974-3980`:

```c
s32 xoff = _SHIFTR((xoffset+0x156),1,24);
s32 yoff = _SHIFTR((yoffset+0x156),1,24);
GX_LOAD_BP_REG((0x59000000|(_SHIFTL(yoff,10,10))|(xoff&0x3ff)));
```

| Bits | Field |
|---|---|
| 9:0 | X offset: `(xoffset + 342) / 2` — i.e. in units of 2 pixels |
| 19:10 | Y offset: `(yoffset + 342) / 2` |
| 23:20 | **Unverified:** not written. |

Cross-checked: Dolphin uses the same x-low/y-high 10+10 packing for this register.

---

## 4. Z mode and PE control

### 4.1 Z mode (`0x40`)

Register number from `gx.c:1133`. Written by `GX_SetZMode()` (`gx.c:2948-2954`):

```c
__gx->peZMode = (__gx->peZMode&~0x1)|(enable&1);
__gx->peZMode = (__gx->peZMode&~0xe)|(_SHIFTL(func,1,3));
__gx->peZMode = (__gx->peZMode&~0x10)|(_SHIFTL(update_enable,4,1));
```

| Bits | Field |
|---|---|
| 0 | Depth test enable |
| 3:1 | Compare function (NEVER, LESS, EQUAL, LEQUAL, GREATER, NEQUAL, GEQUAL, ALWAYS = 0..7) |
| 4 | Depth write (update) enable |
| 23:5 | **Unverified:** not written. |

Corroborating use: to force a clear-pass, `GX_CopyDisp()` writes `(__gx->peZMode&~0xf)|0xf`
(`gx.c:2016`) — test enable set with function 7 (ALWAYS), which is exactly what an
unconditional-write clear needs.

Cross-checked against Dolphin: identical.

### 4.2 PE control (`0x43`)

Register number from `gx.c:1136`. Written by `GX_SetPixelFmt()` and `GX_SetZCompLoc()`.

| Bits | Field | Evidence |
|---|---|---|
| 2:0 | EFB pixel format | `__gx->peCntrl = (__gx->peCntrl&~0x7)\|(realfmt[pix_fmt]&0x7);` — `gx.c:4399` |
| 5:3 | Depth (Z) format | `(_SHIFTL(z_fmt,3,3))` — `gx.c:4400` |
| 6 | Early Z: depth test before texturing | `(_SHIFTL(before_tex,6,1))` — `gx.c:4391` |
| 23:7 | **Unverified:** not written. |

**Pixel format encoding.** libogc maps the API enum through
`u32 realfmt[8] = {0,1,2,3,4,4,4,5};` (`gx.c:4398`), so the eight API formats collapse onto six
hardware values 0..5. API values 4, 5, 6 (the Y8 / U8 / V8 intensity variants) all become hardware
value 4, and the *which* of the three is then selected by bits 11:10 of register `0x42` (see
§6.2). Value 3 is the Z24 format: `GX_CopyDisp()` tests `(__gx->peCntrl&0x7)==0x0003` to detect it
(`gx.c:2023`), and `GX_CopyTex()` forces the field to 3 when copying a Z texture (`gx.c:2068`).

Cross-checked against Dolphin: same three field positions.

---

## 5. Colour mode / blend

### 5.1 Colour mode 0 / blend (`0x41`)

Register number from `gx.c:1134`. Written by `GX_SetBlendMode()` (`gx.c:3735-3752`),
`GX_SetColorUpdate()`, `GX_SetAlphaUpdate()`, `GX_SetDither()`.

| Bits | Field | Evidence |
|---|---|---|
| 0 | Blend enable | set for `GX_BM_BLEND` and `GX_BM_SUBTRACT` — `gx.c:3737-3738` |
| 1 | Logic-op enable | set for `GX_BM_LOGIC` — `gx.c:3743-3744` |
| 2 | Dither enable | `(_SHIFTL(dither,2,1))` — `gx.c:4417` |
| 3 | Colour update (write colour to EFB) | `(_SHIFTL(enable,3,1))` — `gx.c:4379` |
| 4 | Alpha update (write alpha to EFB) | `(_SHIFTL(enable,4,1))` — `gx.c:4385` |
| 7:5 | Destination blend factor | `(_SHIFTL(dst_fact,5,3))` — `gx.c:3748` |
| 10:8 | Source blend factor | `(_SHIFTL(src_fact,8,3))` — `gx.c:3749` |
| 11 | Subtract mode (dst − src instead of the weighted sum) | set only for `GX_BM_SUBTRACT` — `gx.c:3740-3741` |
| 15:12 | Logic operation | `(_SHIFTL(op,12,4))` — `gx.c:3747` |
| 23:16 | **Unverified:** not written. |

Note that bits 0, 1 and 11 together encode the blend *mode*: none (all clear), blend (bit 0),
logic (bit 1), subtract (bits 0 and 11). A blend-disable fast path in the copy code writes
`(__gx->peCMode0&~0x3)` — clearing both bit 0 and bit 1 (`gx.c:2018`).

Cross-checked against Dolphin: identical field positions, including the subtract bit at 11.

The same layout is reused for the CPU-side PE "poke" register (`GX_PokeBlendMode()`,
`gx.c:4476-4494`), which OR-s in `0x41000000` before storing to `_peReg[1]` — independent
confirmation that `0x41` is this layout.

---

## 6. Alpha compare and destination alpha

### 6.1 Alpha compare (`0xF3`)

`gx.c:4039-4044`:

```c
val = (_SHIFTL(aop,22,2))|(_SHIFTL(comp1,19,3))|(_SHIFTL(comp0,16,3))|(_SHIFTL(ref1,8,8))|(ref0&0xff);
GX_LOAD_BP_REG(0xf3000000|val);
```

| Bits | Field |
|---|---|
| 7:0 | Reference value 0 |
| 15:8 | Reference value 1 |
| 18:16 | Compare function 0 (same 0..7 encoding as the Z compare function) |
| 21:19 | Compare function 1 |
| 23:22 | Logic combining the two results: 0 = AND, 1 = OR, 2 = XOR, 3 = XNOR |

The logic-op encoding is the libogc/GX `GX_AOP_*` enum order; the register simply carries the
enum value, so the numbering above is the API's, which libogc passes through unmodified.
**Unverified:** the AND/OR/XOR/XNOR ordering itself is not provable from `gx.c` alone (the enum
lives in `gx.h`, which was not part of this analysis) — the *bit position* 23:22 is certain.

Cross-checked against Dolphin: same five fields at the same positions.

### 6.2 Destination alpha (`0x42`)

Register number from `gx.c:1135`. Written by `GX_SetDstAlpha()` (`gx.c:4421-4427`):

```c
__gx->peCMode1 = (__gx->peCMode1&~0xff)|(a&0xff);
__gx->peCMode1 = (__gx->peCMode1&~0x100)|(_SHIFTL(enable,8,1));
```

| Bits | Field |
|---|---|
| 7:0 | Constant alpha value written to the EFB |
| 8 | Constant-alpha enable |
| 9 | **Unverified:** not written. |
| 11:10 | Intensity sub-format selector, used only when the PE pixel format is the Y8/U8/V8 family |
| 23:12 | **Unverified:** not written. |

Evidence for 11:10 (`gx.c:4406-4410`):

```c
if(realfmt[pix_fmt]==GX_PF_Y8) {
    pix_fmt -= GX_PF_Y8;
    __gx->peCMode1 = (__gx->peCMode1&~0xC00)|(_SHIFTL(pix_fmt,10,2));
    GX_LOAD_BP_REG(__gx->peCMode1);
}
```

so the field carries 0 for Y8, 1 for U8, 2 for V8 (the API enum offset from `GX_PF_Y8`).

**Unverified:** Dolphin's trimmed header defines only the 8-bit alpha and the enable bit for this
register, so bits 11:10 could not be cross-checked. The libogc evidence above is direct and
unambiguous, but no second source confirms it.

---

## 7. TEV

libogc keeps a shadow array per TEV register family; the register numbers come from the
initialisation loops in `GX_Init()`.

### 7.1 TEV colour combiner (`0xC0` + 2n)

Register numbers from `gx.c:1104-1111`: `re0 = 0xc0;` then `re0 += 2` for each of 16 stages. So
stage *n* uses register `0xC0 + 2n` (`0xC0, 0xC2, ... 0xDE`).

Written by `GX_SetTevColorIn()` (`gx.c:3871-3880`) and `GX_SetTevColorOp()` (`gx.c:3893-3909`).

| Bits | Field | Evidence |
|---|---|---|
| 3:0 | Input D (10-bit precision input) | `(__gx->tevColorEnv[reg]&~0xf)\|(d&0xf)` — `gx.c:3877` |
| 7:4 | Input C | `(_SHIFTL(c,4,4))` — `gx.c:3876` |
| 11:8 | Input B | `(_SHIFTL(b,8,4))` — `gx.c:3875` |
| 15:12 | Input A | `(_SHIFTL(a,12,4))` — `gx.c:3874` |
| 17:16 | Bias (value 3 selects compare mode) | `(_SHIFTL(tevbias,16,2))` — `gx.c:3900` |
| 18 | Operation: 0 = add, 1 = subtract. In compare mode, selects which of a pair of comparisons | `(_SHIFTL(tevop,18,1))` — `gx.c:3896` |
| 19 | Clamp result to 0..1 | `(_SHIFTL(clamp,19,1))` — `gx.c:3905` |
| 21:20 | Output scale (×1, ×2, ×4, ÷2). In compare mode, the comparison operand size | `(_SHIFTL(tevscale,20,2))` — `gx.c:3899` |
| 23:22 | Destination TEV register (prev, reg0, reg1, reg2) | `(_SHIFTL(tevregid,22,2))` — `gx.c:3906` |

**Compare mode.** When the API op is above `GX_TEV_SUB`, libogc forces bias to 3 and folds the
op number into bits 21:20 (`gx.c:3902-3903`):

```c
__gx->tevColorEnv[reg] = (__gx->tevColorEnv[reg]&~0x300000)|((_SHIFTL(tevop,19,4))&0x300000);
__gx->tevColorEnv[reg] = (__gx->tevColorEnv[reg]&~0x30000)|0x30000;
```

So: **bias == 3 means the stage is a comparison**, bits 21:20 select which comparison
(R8 / GR16 / BGR24 / RGB8-per-component), and bit 18 selects GT vs EQ. Cross-checked: Dolphin
aliases the same bits as `comparison` and `compare_mode` under the same "bias is compare"
condition.

### 7.2 TEV alpha combiner (`0xC1` + 2n)

Register numbers from `gx.c:1105-1111`: `re1 = 0xc1;` then `re1 += 2`. Stage *n* uses
`0xC1 + 2n` (`0xC1, 0xC3, ... 0xDF`).

Written by `GX_SetTevAlphaIn()` (`gx.c:3882-3891`), `GX_SetTevAlphaOp()` (`gx.c:3911-3927`) and
`GX_SetTevSwapMode()` (`gx.c:4068-4074`).

| Bits | Field | Evidence |
|---|---|---|
| 1:0 | Rasterised-colour swap table index for this stage | `(__gx->tevAlphaEnv[reg]&~0x3)\|(ras_sel&0x3)` — `gx.c:4071` |
| 3:2 | Texture-colour swap table index for this stage | `(_SHIFTL(tex_sel,2,2))` — `gx.c:4072` |
| 6:4 | Input D | `(_SHIFTL(d,4,3))` — `gx.c:3888` |
| 9:7 | Input C | `(_SHIFTL(c,7,3))` — `gx.c:3887` |
| 12:10 | Input B | `(_SHIFTL(b,10,3))` — `gx.c:3886` |
| 15:13 | Input A | `(_SHIFTL(a,13,3))` — `gx.c:3885` |
| 17:16 | Bias (3 = compare mode) | `(_SHIFTL(tevbias,16,2))` — `gx.c:3918` |
| 18 | Operation: add / subtract, or comparison selector | `(_SHIFTL(tevop,18,1))` — `gx.c:3914` |
| 19 | Clamp | `(_SHIFTL(clamp,19,1))` — `gx.c:3923` |
| 21:20 | Scale, or compare mode | `(_SHIFTL(tevscale,20,2))` — `gx.c:3917` |
| 23:22 | Destination TEV register | `(_SHIFTL(tevregid,22,2))` — `gx.c:3924` |

Note the alpha inputs are 3 bits, not 4 — the alpha combiner has 8 possible inputs where the
colour combiner has 16. The top four bits of the colour register are inputs; the bottom four bits
of the alpha register are the swap-table selectors instead.

Cross-checked against Dolphin: identical, including the swap selectors at 1:0 and 3:2.

### 7.3 TEV stage order (`0x28`–`0x2F`)

Register numbers from `gx.c:1138-1143`: the `tevRasOrder[]` shadow array starts at `re0 = 0x25`
and covers 11 registers, `0x25`..`0x2F`. Indices 0 and 1 are the indirect coordinate scale
registers (§11.4), index 2 is the indirect texture order register (§11.5), and indices 3..10 are
the stage-order registers `0x28`..`0x2F`.

`GX_SetTevOrder()` computes `u32 reg = 3+(_SHIFTR(tevstage,1,3));` (`gx.c:3992`), so **each
register holds two consecutive TEV stages**: register `0x28 + k` holds stages `2k` (even) and
`2k + 1` (odd).

| Bits | Field (even stage, `tevstage & 1 == 0`) | Evidence |
|---|---|---|
| 2:0 | Texture map index | `(__gx->tevRasOrder[reg]&~0x7)\|(texm&0x7)` — `gx.c:4014` |
| 5:3 | Texture coordinate index | `(_SHIFTL(texc,3,3))` — `gx.c:4015` |
| 6 | Texture enable (0 when the stage uses no texture) | `(_SHIFTL(tmp,6,1))` — `gx.c:4021` |
| 9:7 | Rasterised colour channel | `(_SHIFTL(colid,7,3))` — `gx.c:4019` |
| 11:10 | **Unverified:** not written; presumed unused. |

| Bits | Field (odd stage, `tevstage & 1 == 1`) | Evidence |
|---|---|---|
| 14:12 | Texture map index | `(_SHIFTL(texm,12,3))` — `gx.c:4005` |
| 17:15 | Texture coordinate index | `(_SHIFTL(texc,15,3))` — `gx.c:4006` |
| 18 | Texture enable | `(_SHIFTL(tmp,18,1))` — `gx.c:4012` |
| 21:19 | Rasterised colour channel | `(_SHIFTL(colid,19,3))` — `gx.c:4010` |
| 23:22 | **Unverified:** not written; presumed unused. |

Corroboration for the read direction: `__GX_SetSUTexRegs()` recovers the texcoord with
`_SHIFTR(__gx->tevRasOrder[tevreg],15,3)` for odd stages and `_SHIFTR(...,3,3)` for even
(`gx.c:912-913`).

The texture-enable bit is cleared when the map is `GX_TEXMAP_NULL` or has the `0x100` flag set
(`gx.c:4011, 4020`). The colour channel field takes the value `GX_ALPHA_BUMP` when the stage's
colour input is `GX_COLORNULL` (`gx.c:4008, 4017`).

Cross-checked against Dolphin: same two-stages-per-register packing at the same bit positions.

### 7.4 TEV konstant select / swap table (`0xF6`–`0xFD`)

Register numbers from `gx.c:1155-1160`: `re0 = 0xf6;` for 8 registers, `0xF6`..`0xFD`.

These eight registers serve **two purposes at once**, which is the single most confusing thing
about the BP TEV state:

1. Bits 3:0 of register `0xF6 + k` hold two entries of the **colour swap table**. The swap table
   has 4 entries of 4 components each; entry *s* occupies registers `0xF6 + 2s` (red, green) and
   `0xF6 + 2s + 1` (blue, alpha).
2. Bits 23:4 hold the **konstant colour and konstant alpha selectors** for TEV stages `2k` and
   `2k + 1`.

| Bits | Field | Evidence |
|---|---|---|
| 1:0 | Swap table component (red for even register, blue for odd) | `(&~0x3)\|(r&0x3)` / `(&~0x3)\|(b&0x3)` — `gx.c:4080, 4083` |
| 3:2 | Swap table component (green for even register, alpha for odd) | `(_SHIFTL(g,2,2))` / `(_SHIFTL(a,2,2))` — `gx.c:4081, 4084` |
| 8:4 | Konstant **colour** selector, even stage (`2k`) | `(_SHIFTL(sel,4,5))` — `gx.c:4053` |
| 13:9 | Konstant **alpha** selector, even stage | `(_SHIFTL(sel,9,5))` — `gx.c:4064` |
| 18:14 | Konstant **colour** selector, odd stage (`2k + 1`) | `(_SHIFTL(sel,14,5))` — `gx.c:4051` |
| 23:19 | Konstant **alpha** selector, odd stage | `(_SHIFTL(sel,19,5))` — `gx.c:4062` |

`GX_SetTevSwapModeTable()` derives the register pair as `regA = 0+(_SHIFTL(swapid,1,3))` and
`regB = 1+(_SHIFTL(swapid,1,3))` (`gx.c:4078-4079`) — i.e. array indices `2*swapid` and
`2*swapid + 1`, which are registers `0xF6 + 2*swapid` and `0xF6 + 2*swapid + 1`.

`GX_SetTevKColorSel()` / `GX_SetTevKAlphaSel()` derive the register as
`reg = (_SHIFTR(tevstage,1,3))` (`gx.c:4048, 4059`) — array index `tevstage / 2`.

Each swap-table component field is a 2-bit source selector (0 = R, 1 = G, 2 = B, 3 = A).

Cross-checked against Dolphin: identical layout, including the dual use of one register file.

### 7.5 TEV colour and konstant registers (`0xE0`–`0xE7`)

Four TEV colour registers (prev, reg0, reg1, reg2) and four konstant registers share the same
eight register numbers, discriminated by a type bit.

`GX_SetTevColor()` (`gx.c:3775-3788`):

```c
reg = (_SHIFTL((0xe0+(tev_regid<<1)),24,8)|(_SHIFTL(color.a,12,8))|(color.r&0xff));
GX_LOAD_BP_REG(reg);
reg = (_SHIFTL((0xe1+(tev_regid<<1)),24,8)|(_SHIFTL(color.g,12,8))|(color.b&0xff));
GX_LOAD_BP_REG(reg);
```

`GX_SetTevKColor()` (`gx.c:3805-3818`) is the same but ORs in `(_SHIFTL(1,23,1))`.

**Register `0xE0 + 2n` — red / alpha half of register *n*:**

| Bits | Field |
|---|---|
| 10:0 | Red, signed 11-bit (`GX_SetTevColorS10` writes the full 11 bits, `gx.c:3794`; the 8-bit entry point writes only `color.r & 0xff`) |
| 11 | **Unverified:** not written by either entry point. |
| 22:12 | Alpha, signed 11-bit |
| 23 | Register type: 0 = TEV colour register, 1 = konstant register |

**Register `0xE1 + 2n` — blue / green half of register *n*:**

| Bits | Field |
|---|---|
| 10:0 | Blue, signed 11-bit |
| 11 | **Unverified:** not written. |
| 22:12 | Green, signed 11-bit |
| 23 | Register type, as above |

So the eight register numbers are `0xE0`/`0xE1` (n=0), `0xE2`/`0xE3` (n=1), `0xE4`/`0xE5` (n=2),
`0xE6`/`0xE7` (n=3), and *the same eight numbers* address the konstant registers when bit 23 is
set.

Note the odd component order: red and alpha travel together, blue and green travel together.

libogc emits each pair twice more as a write-gather-pipe flush (`gx.c:3785-3787`); those duplicate
writes carry no new state and an emulator can ignore the repetition (it is idempotent anyway).

Cross-checked against Dolphin: same split, same type bit at 23, same 11-bit signed fields.

---

## 8. Texture

libogc builds most texture registers inside a `GXTexObj` and only stamps the register number in at
`GX_LoadTexObjPreloaded()` time, using these tables (`gx.c:101-107`):

```c
static const u8 _gxtexmode0ids[8] = {0x80,0x81,0x82,0x83,0xA0,0xA1,0xA2,0xA3};
static const u8 _gxtexmode1ids[8] = {0x84,0x85,0x86,0x87,0xA4,0xA5,0xA6,0xA7};
static const u8 _gxteximg0ids[8]  = {0x88,0x89,0x8A,0x8B,0xA8,0xA9,0xAA,0xAB};
static const u8 _gxteximg1ids[8]  = {0x8C,0x8D,0x8E,0x8F,0xAC,0xAD,0xAE,0xAF};
static const u8 _gxteximg2ids[8]  = {0x90,0x91,0x92,0x93,0xB0,0xB1,0xB2,0xB3};
static const u8 _gxteximg3ids[8]  = {0x94,0x95,0x96,0x97,0xB4,0xB5,0xB6,0xB7};
static const u8 _gxtextlutids[8]  = {0x98,0x99,0x9A,0x9B,0xB8,0xB9,0xBA,0xBB};
```

The index is the texture map id 0..7. **Texture units 0-3 live in the `0x80`–`0x9F` block and
units 4-7 in the `0xA0`–`0xBB` block**, with identical layouts. The dispatch is at
`gx.c:3450-3472`.

### 8.1 Texture setmode0 (`0x80`–`0x83`, `0xA0`–`0xA3`)

Built in `GX_InitTexObj()` (`gx.c:3236-3248`) and `GX_InitTexObjLOD()` (`gx.c:3317-3322`) as
`ptr->tex_filt`.

| Bits | Field | Evidence |
|---|---|---|
| 1:0 | Wrap mode S (0 = clamp, 1 = repeat, 2 = mirror) | `(ptr->tex_filt&~0x03)\|(wrap_s&3)` — `gx.c:3236` |
| 3:2 | Wrap mode T | `(_SHIFTL(wrap_t,2,2))` — `gx.c:3237` |
| 4 | Magnification filter: 0 = near, 1 = linear | `(_SHIFTL((magfilt==GX_LINEAR?1:0),4,1))` — `gx.c:3318` |
| 6:5 | Mip filter mode | see below |
| 7 | Minification filter: 0 = near, 1 = linear | see below |
| 8 | Diagonal LOD / edge LOD select | `(_SHIFTL(!(edgelod&0xff),8,1))` — `gx.c:3320` (note the logical NOT: the register bit is the *inverse* of the API's `edgelod`) |
| 16:9 | LOD bias, signed 8-bit, in 1/32 steps | `(_SHIFTL(((u32)(32.0f*lodbias)),9,8))` — `gx.c:3317`; clamped to [-4.0, 3.99] at `gx.c:3314-3315` |
| 18:17 | **Unverified:** not written by libogc. |
| 20:19 | Max anisotropy | `(_SHIFTL(maxaniso,19,2))` — `gx.c:3321` |
| 21 | LOD bias clamp | `(_SHIFTL(biasclamp,21,1))` — `gx.c:3322` |
| 23:22 | **Unverified:** not written. |

Bits 7:5 are written as one 3-bit value by libogc:
`(ptr->tex_filt&~0xe0)|(_SHIFTL(_gx2HWFiltConv[minfilt],5,3))` (`gx.c:3319`), where
`static const u8 _gx2HWFiltConv[] = {0x00,0x04,0x01,0x05,0x02,0x06,0x00,0x00};` (`gx.c:108`).
Reading that table, the low two bits (register bits 6:5) are the mip mode 0/1/2 and bit 2 of the
table value (register bit 7) is the linear-minification flag — the API's NEAR/LINEAR alternate
while NEAR_MIP_NEAR/LINEAR_MIP_NEAR/... step the mip mode. Cross-checked: Dolphin splits the same
three bits into a 2-bit mip mode at 6:5 and a 1-bit min filter at 7.

The direct-construction paths corroborate: a non-mipmapped texture gets `0x0080` (bit 7 set, mip
mode 0), a mipmapped CI texture gets `0x00a0` (mip mode 1), and other mipmapped textures get
`0x00c0` (mip mode 2) — `gx.c:3241-3247`.

### 8.2 Texture setmode1 (`0x84`–`0x87`, `0xA4`–`0xA7`)

`ptr->tex_lod`, built in `GX_InitTexObjLOD()` (`gx.c:3330-3331`).

| Bits | Field |
|---|---|
| 7:0 | Minimum LOD, in 1/16 steps (`(u32)(16.0f*minlod)`), clamped to 0..10.0 |
| 15:8 | Maximum LOD, in 1/16 steps |
| 23:16 | **Unverified:** not written. |

Cross-checked against Dolphin: same two 8-bit fields.

### 8.3 Texture setimage0 (`0x88`–`0x8B`, `0xA8`–`0xAB`)

`ptr->tex_size`, built in `GX_InitTexObj()` (`gx.c:3250-3252`).

| Bits | Field |
|---|---|
| 9:0 | Width **minus one** |
| 19:10 | Height **minus one** |
| 23:20 | Texture format (4 bits) |

Read back at `gx.c:3026` (`_SHIFTR(... & 0xffc00, 10, 10) + 1` for height) and `gx.c:3506`
(`_SHIFTR(ptr->tex_size,20,4)` for format), which confirms all three fields.

### 8.4 Texture setimage1, 2, 3

**setimage1 (`0x8C`–`0x8F`, `0xAC`–`0xAF`)** — even-LOD TMEM configuration.
Built as `ptr->tmem_even` in `GX_InitTexCacheRegion()` (`gx.c:3168-3171`) and
`GX_InitTexPreloadRegion()` (`gx.c:3200-3201`).

| Bits | Field | Evidence |
|---|---|---|
| 14:0 | TMEM line index for even LODs — the byte address **shifted right by 5** | `(_SHIFTR(tmem_even,5,15))` — `gx.c:3169` |
| 17:15 | Cache width | `(_SHIFTL(sze,15,3))` — `gx.c:3170` |
| 20:18 | Cache height | `(_SHIFTL(sze,18,3))` — `gx.c:3171` |
| 21 | Cache is managed manually (preloaded) rather than auto-fetched | `(ptr->tmem_even&~0x200000)\|0x200000` in the *preload* region init — `gx.c:3200` |
| 23:22 | **Unverified:** not written. |

The cache size code `sze` is 3 for 32K, 4 for 128K, 5 for 512K (`gx.c:3149-3167`).

**setimage2 (`0x90`–`0x93`, `0xB0`–`0xB3`)** — odd-LOD TMEM configuration. Identical layout to
setimage1 for bits 20:0 (`gx.c:3188-3190`), but libogc never sets bit 21 on this register.

**setimage3 (`0x94`–`0x97`, `0xB4`–`0xB7`)** — image base address.

| Bits | Field |
|---|---|
| 23:0 | Physical address of the texture data, **shifted right by 5** (`_SHIFTR(MEM_VIRTUAL_TO_PHYSICAL(img_ptr),5,24)` — `gx.c:3253`) |

The inverse is visible at `gx.c:3011`: `(void*)(_SHIFTL(... & 0x00ffffff,5,24))`.

Cross-checked against Dolphin: all four setimage layouts agree.

### 8.5 SU texture size registers (`0x30`–`0x3F`)

Register numbers from `gx.c:1126-1131`: `re0 = 0x30; re1 = 0x31;` stepping by 2 for 8 entries. So
texture **coordinate** *n* (not texture map) uses S-size register `0x30 + 2n` and T-size register
`0x31 + 2n`, for n = 0..7.

These are set from the bound texture's dimensions by `__SetSURegs()` (`gx.c:846-865`), indexed by
`reg = (texcoord&0x7)`.

| Bits | Field (both S and T registers) | Evidence |
|---|---|---|
| 15:0 | Scale **minus one**: the S register gets `texMapSize & 0x3ff` (width−1) and the T register gets `_SHIFTR(texMapSize,10,10)` (height−1) | `gx.c:853-859` |
| 16 | Range bias / cylindrical-wrap enable | `(_SHIFTL(wrap_s,16,1))` — `gx.c:860`; also `(_SHIFTL(s_enable,16,1))` from `GX_SetTexCoordBias()` — `gx.c:3700` |
| 17 | Cylindrical wrap | `(_SHIFTL(s_enable,17,1))` from `GX_SetTexCoordCylWrap()` — `gx.c:3686` |
| 18 | Line offset (S register only) | `(_SHIFTL(line_enable,18,1))` — `gx.c:3946` |
| 19 | Point offset (S register only) | `(_SHIFTL(point_enable,19,1))` — `gx.c:3947` |
| 23:20 | **Unverified:** not written. |

**Caution on bits 16 and 17.** `__SetSURegs()` writes the *texture wrap mode* into bit 16
(`_SHIFTL(wrap_s,16,1)` — only the low bit of a 2-bit wrap mode survives), while
`GX_SetTexCoordBias()` writes a range-bias flag into the same bit and `GX_SetTexCoordCylWrap()`
writes cylindrical wrap into bit 17. Cross-checking against Dolphin: bit 16 is the range-bias
flag and bit 17 is cylindrical wrap. The `__SetSURegs()` write is libogc conflating "wrap mode is
repeat" with "enable range bias" — plausible behaviour, but an emulator should implement bit 16
as *range bias* per Dolphin, not as a wrap mode.

libogc only auto-updates these registers for texcoords that are not under manual control
(`__gx->texCoordManually`, `gx.c:3670-3679`); `GX_SetTexCoordScaleManually()` writes bits 15:0
directly.

### 8.6 TLUT registers

Three separate registers are involved.

**`0x64` — TLUT source address.** Built in `GX_InitTlutObj()` (`gx.c:3427-3429`):

| Bits | Field |
|---|---|
| 23:0 | Physical address of the palette in main memory, **shifted right by 5** |

**`0x65` — TLUT destination and size.** Built in `GX_InitTlutRegion()` (`gx.c:3218-3223`):

```c
tmem_addr -= 0x80000;
ptr->tmem_addr_conf = (ptr->tmem_addr_conf&~0x3ff)|(_SHIFTR(tmem_addr,9,10));
ptr->tmem_addr_conf = (ptr->tmem_addr_conf&~0x1FFC00)|(_SHIFTL(tlut_sz,10,10));
```

| Bits | Field |
|---|---|
| 9:0 | Destination TMEM address: `(tmem_addr − 0x80000) >> 9`, i.e. in 512-byte units within the TLUT half of TMEM |
| 19:10 | Size / entry count code (`GX_TLUT_256`, `GX_TLUT_1K`, …) |
| 23:20 | **Unverified:** not written. |

Both are emitted together by `GX_LoadTlut()` (`gx.c:3655-3658`), bracketed by texture-state
flushes.

**`0x98`–`0x9B`, `0xB8`–`0xBB` — per-texture-unit TLUT binding.** This is the register that tells
a texture unit *which* TLUT to sample. Assembled at `gx.c:3661`:
`region->tmem_addr_base = (ptr->tlut_fmt&~0x3ff)|(region->tmem_addr_conf&0x3ff);`

| Bits | Field | Evidence |
|---|---|---|
| 9:0 | TMEM offset of the palette (the same 10-bit value as bits 9:0 of register `0x65`) | `gx.c:3661` |
| 11:10 | TLUT format (IA8 / RGB565 / RGB5A3) | `ptr->tlut_fmt = _SHIFTL(fmt,10,2);` — `gx.c:3427` |
| 23:12 | **Unverified:** not written. |

Cross-checked against Dolphin: 10-bit offset plus 2-bit format at the same positions.

### 8.7 Texture preload registers (`0x60`–`0x63`)

Written by `GX_PreloadEntireTexture()` (`gx.c:3493-3512`).

| Reg | Bits | Field |
|---|---|---|
| `0x60` | 23:0 | Source physical address >> 5 (taken straight from the texobj's setimage3 payload) |
| `0x61` | 14:0 | Destination TMEM line for even LODs |
| `0x62` | 14:0 | Destination TMEM line for odd LODs |
| `0x63` | 14:0 | Tile count |
| `0x63` | 16:15 | Tile type (`_SHIFTL(ptr->tex_tile_type,15,2)` — `gx.c:3504`) |

Note that register number `0x61` here is unrelated to the FIFO opcode `0x61`.

Cross-checked against Dolphin: the preload mode register is a 15-bit count plus a 2-bit type at
bit 15.

### 8.8 Texture invalidate (`0x66`)

`GX_InvalidateTexAll()` (`gx.c:3605-3611`) issues two fixed writes:

```c
GX_LOAD_BP_REG(0x66001000);
GX_LOAD_BP_REG(0x66001100);
```

`GX_InvalidateTexRegion()` (`gx.c:3613-3646`) builds:

```c
regvalA = _SHIFTR((ptr->tmem_even&0x7fff),6,9)|(_SHIFTL(size,9,4))|(_SHIFTL(0x66,24,8));
```

| Bits | Field |
|---|---|
| 8:0 | TMEM line index of the region to invalidate, shifted right a further 6 (so the register holds `tmem_byte_addr >> 11`) |
| 12:9 | Size code: a log2-ish measure derived from the region's cache width + height codes |
| 23:13 | **Unverified:** not written. |

**Unverified:** Dolphin's trimmed header carries this register only as an opaque word, so the
field split above rests on libogc alone. The invalidate-all values `0x1000` and `0x1100` fit the
layout (size code 8, line index 0 and 256 respectively), which is consistent but not proof.

---

## 9. The EFB copy

libogc keeps two independent shadow copies of the copy state — `dispCopy*` for
`GX_CopyDisp()`/XFB copies and `texCopy*` for `GX_CopyTex()`/texture copies — but both target the
**same** BP registers. The only structural difference is bit 14 of the trigger.

### 9.1 EFB copy source rectangle (`0x49`, `0x4A`)

`GX_SetDispCopySrc()` (`gx.c:2002-2008`) and `GX_SetTexCopySrc()` (`gx.c:2097-2103`) both use the
`XY(x,y)` macro (`gx.c:64`), which packs x at bits 9:0 and y at bits 19:10.

**`0x49` — source top-left:**

| Bits | Field |
|---|---|
| 9:0 | Left edge (X), in EFB pixels |
| 19:10 | Top edge (Y) |
| 23:20 | **Unverified:** not written. |

**`0x4A` — source width/height:**

| Bits | Field |
|---|---|
| 9:0 | Width **minus one** |
| 19:10 | Height **minus one** |
| 23:20 | **Unverified:** not written. |

**Known inconsistency in libogc.** `GX_SetDispCopyYScale()` reads the height back with
`ht = _SHIFTR(__gx->dispCopyWH,12,10)+1;` (`gx.c:1992`) — bits 21:12, which contradicts the
`XY()` macro that wrote it at bits 19:10. The *write* path is what reaches hardware and both copy
paths use `XY()`, so **implement height at bits 19:10**. The bits-12 read affects only libogc's
returned XFB line count and appears to be a bug in the library. Dolphin's trimmed header does not
contain the field declaration, so this could not be cross-checked from the second source.

### 9.2 EFB copy destination (`0x4B`, `0x4D`)

**`0x4B` — destination address** (`gx.c:2036`, `gx.c:2078`):

```c
val = 0x4b000000|(_SHIFTR(MEM_VIRTUAL_TO_PHYSICAL(dest),5,24));
```

| Bits | Field |
|---|---|
| 23:0 | Destination physical address in main memory, **shifted right by 5** (32-byte aligned) |

**`0x4D` — destination stride:**

| Bits | Field | Evidence |
|---|---|---|
| 9:0 | Stride. For an XFB copy: `width >> 4` (`_SHIFTR(wd,4,10)` — `gx.c:1998`). For a texture copy: `xtiles * zplanes` (`gx.c:2111`) |
| 23:10 | **Unverified:** not written. |

`zplanes` is 2 for `GX_TF_RGBA8` and `GX_TF_Z24X8` and 1 otherwise (`gx.c:1887`), reflecting that
those formats occupy two tile planes.

### 9.3 Copy vertical scale (`0x4E`)

`gx.c:1986-1989`:

```c
yScale = ((u32)(256.0f/yscale))&0x1ff;
GX_LOAD_BP_REG(0x4e000000|yScale);
```

| Bits | Field |
|---|---|
| 8:0 | Vertical scale step, as `256 / yscale` — i.e. a 1.8 fixed-point reciprocal; 256 means 1:1 |
| 23:9 | **Unverified:** not written. |

When this value is anything other than 256, bit 10 of the trigger register must also be set
(§9.7); libogc does this at `gx.c:1991`.

### 9.4 Copy clear colour and Z (`0x4F`–`0x51`)

`GX_SetCopyClear()` (`gx.c:1891-1903`):

| Reg | Bits | Field |
|---|---|---|
| `0x4F` | 7:0 | Clear red |
| `0x4F` | 15:8 | Clear alpha |
| `0x50` | 7:0 | Clear blue |
| `0x50` | 15:8 | Clear green |
| `0x51` | 23:0 | Clear Z value (24-bit) |

Note the same red-with-alpha / blue-with-green pairing as the TEV colour registers (§7.5).

### 9.5 Copy filter — sample pattern (`0x01`–`0x04`)

`GX_SetCopyFilter()` (`gx.c:1916-1962`) writes four registers holding twelve (x, y) sample
positions, three pairs per register, each component 4 bits:

| Bits | Field |
|---|---|
| 3:0 | Sample *3k+0* X |
| 7:4 | Sample *3k+0* Y |
| 11:8 | Sample *3k+1* X |
| 15:12 | Sample *3k+1* Y |
| 19:16 | Sample *3k+2* X |
| 23:20 | Sample *3k+2* Y |

where register `0x01 + k` holds samples `3k`..`3k+2`, k = 0..3.

When anti-aliasing is off libogc writes the fixed values `0x01666666`, `0x02666666`,
`0x03666666`, `0x04666666` (`gx.c:1954-1957`) — i.e. every component 6, the centre of the 4-bit
range.

### 9.6 Copy vertical filter (`0x53`, `0x54`)

`gx.c:1964-1976`. Seven 6-bit weights across two registers:

| Reg | Bits | Field |
|---|---|---|
| `0x53` | 5:0 | Weight 0 |
| `0x53` | 11:6 | Weight 1 |
| `0x53` | 17:12 | Weight 2 |
| `0x53` | 23:18 | Weight 3 |
| `0x54` | 5:0 | Weight 4 |
| `0x54` | 11:6 | Weight 5 |
| `0x54` | 17:12 | Weight 6 |
| `0x54` | 23:18 | **Unverified:** not written. |

The unfiltered defaults are `0x53595000` and `0x54000015` (`gx.c:1965-1966`): weights 0, 0,
0x15 (21), 0x16 (22), 0x15 (21), 0, 0 — summing to 64.

### 9.7 EFB copy trigger (`0x52`)

This is the register that actually starts the copy. Assembled in `GX_CopyDisp()`
(`gx.c:2039-2043`) and `GX_CopyTex()` (`gx.c:2085-2088`) from the `dispCopyCntrl` / `texCopyCntrl`
shadows.

| Bits | Field | Evidence |
|---|---|---|
| 0 | Clamp top | `(__gx->dispCopyCntrl&~1)\|(clamp&1)` — `gx.c:1907` |
| 1 | Clamp bottom | `(__gx->dispCopyCntrl&~2)\|(clamp&2)` — `gx.c:1908` |
| 2 | **Unverified:** never written by libogc. Dolphin also records it as unknown. |
| 6:3 | Destination pixel format, 4 bits — **with the bits rotated**, see below | `gx.c:2117, 2119` |
| 8:7 | Gamma correction (0 = 1.0, 1 = 1.7, 2 = 2.2) | `(_SHIFTL(gamma,7,2))` — `gx.c:1913` |
| 9 | Half scale (box filter 2:1 instead of 1:1). libogc drives this from its `mipmap` argument | `(_SHIFTL(mipmap,9,1))` — `gx.c:2118` |
| 10 | Vertical scaling active | `(_SHIFTL(((256-yScale)>0),10,1))` — `gx.c:1991` |
| 11 | Clear the EFB region after copying | `(_SHIFTL(clear,11,1))` — `gx.c:2039`, `gx.c:2085` |
| 13:12 | Frame-to-field mode | `(_SHIFTL(mode,12,2))` — `gx.c:1981` |
| 14 | **Copy to XFB** (1) vs **copy to texture** (0) | set at `gx.c:2040` in `GX_CopyDisp()`, cleared at `gx.c:2086` in `GX_CopyTex()` |
| 15 | Intensity format (I4/I8/IA4/IA8) | `gx.c:2114-2115` |
| 16 | Automatic colour conversion | `gx.c:2114-2115` |
| 23:17 | **Unverified:** not written. |

**Bits 15 and 16 together.** `GX_SetTexCopyDst()` writes both at once:

```c
if(fmt==GX_CTF_YUVA8 || (fmt>=GX_TF_I4 && fmt<GX_TF_RGB565))
    __gx->texCopyCntrl = (__gx->texCopyCntrl&~0x18000)|0x18000;
else
    __gx->texCopyCntrl = (__gx->texCopyCntrl&~0x18000)|0x10000;
```

So bit 16 is always set for a texture copy, and bit 15 additionally for intensity/YUV formats.

**The rotated format field.** libogc splits the write (`gx.c:2117, 2119`):

```c
__gx->texCopyCntrl = (__gx->texCopyCntrl&~0x8)|(lfmt&0x8);
__gx->texCopyCntrl = (__gx->texCopyCntrl&~0x70)|(_SHIFTL(lfmt,4,3));
```

Bit 3 of the *format* lands in register bit 3, and format bits 2:0 land in register bits 6:4.
Reading the 4-bit field as a unit, the register value is `((fmt & 7) << 1) | ((fmt & 8) >> 3)` —
a one-position rotate-right of the format code. To recover the format from a captured register
value: `fmt = (field >> 1) | ((field & 1) << 3)`.

Cross-checked against Dolphin, which describes the same cycling-right-shift relationship and the
same bit positions for every field above. `GX_TF_Z16` is remapped to format code 11 before
encoding (`gx.c:2113`).

**Copy-time state overrides.** Both copy entry points temporarily override other registers around
the trigger and restore them afterwards, which an emulator does not need to model specially but
which explains otherwise-mysterious register traffic:

* With `clear` set, Z mode is forced to `(peZMode & ~0xf) | 0xf` and blend is forced off with
  `(peCMode0 & ~0x3)`, then both are restored (`gx.c:2016-2018`, `gx.c:2046-2048`).
* Early-Z (PE control bit 6) is temporarily cleared when clearing or when the pixel format is Z24
  (`gx.c:2022-2029`, `gx.c:2065-2076`).
* `GX_CopyTex()` forces the PE pixel format to 3 when copying a Z texture (`gx.c:2067-2070`).

---

## 10. PE token, PE finish, draw done

### 10.1 PE token (`0x47`, `0x48`)

`GX_SetDrawSync()` (`gx.c:1651-1659`):

```c
GX_LOAD_BP_REG(0x48000000 | token);
GX_LOAD_BP_REG(0x47000000 | token);
```

| Reg | Bits | Field |
|---|---|---|
| `0x48` | 15:0 | Token value that, when reached, **raises the PE token interrupt** |
| `0x47` | 15:0 | Token value written to the PE token readback register (no interrupt) |
| both | 23:16 | **Unverified:** not written. |

The order matters: the interrupt-arming register is written first, then the plain one. The value
is read back from the PE MMIO register (`_peReg[7]`, `gx.c:1663`).

### 10.2 Draw done (`0x45`)

`GX_SetDrawDone()` (`gx.c:1666-1674`) and `GX_DrawDone()` (`gx.c:1688-1700`) both issue exactly:

```c
GX_LOAD_BP_REG(0x45000002); // set draw done!
```

| Bits | Field |
|---|---|
| 23:0 | Command word. The only value libogc ever writes is **2**, which raises the PE finish interrupt once the pipeline drains. |

**Unverified:** other values of this payload. libogc uses only `2`; no other encoding is
observable from this source.

### 10.3 Pipeline sync

Two related flushes, included because they appear constantly in real command streams:

* `GX_PixModeSync()` (`gx.c:1738-1741`) re-writes register `0x43` (PE control) with its current
  value. This is a pixel-pipeline flush, not a state change.
* `GX_TexModeSync()` (`gx.c:1743-1746`) writes `0x63000000` — the preload-mode register with a
  zero payload, used purely as a texture-pipeline barrier. The internal
  `__GX_FlushTextureState()` does the same and brackets every TLUT/preload/invalidate operation.

An emulator must treat a write of `0x63` with payload 0 as a sync, not as a real preload
configuration.

---

## 11. Indirect texture registers

### 11.1 Indirect command (`0x10`–`0x1F`)

One register per TEV stage: register `0x10 + tevstage`. From `GX_SetTevIndirect()`
(`gx.c:4090-4094`):

```c
u32 val = (0x10000000|(_SHIFTL(tevstage,24,4)))|(indtexid&3)|(_SHIFTL(format,2,2))
        |(_SHIFTL(bias,4,3))|(_SHIFTL(a,7,2))|(_SHIFTL(mtxid,9,4))|(_SHIFTL(wrap_s,13,3))
        |(_SHIFTL(wrap_t,16,3))|(_SHIFTL(utclod,19,1))|(_SHIFTL(addprev,20,1));
```

| Bits | Field |
|---|---|
| 1:0 | Indirect texture stage id (0..3) |
| 3:2 | Indirect texture format (8/5/4/3 bits per component) |
| 6:4 | Bias select — three independent enables for S, T, U (bit 4 = S, bit 5 = T, bit 6 = U) |
| 8:7 | Bump-alpha component select (off / S / T / U) |
| 12:9 | Indirect matrix selector, 4 bits |
| 15:13 | Wrap factor for S of the regular coordinate |
| 18:16 | Wrap factor for T of the regular coordinate |
| 19 | Use unmodified texture coordinates for LOD computation |
| 20 | Add the previous TEV stage's texture coordinate result |
| 23:21 | **Unverified:** not written. |

**The 4-bit matrix selector at 12:9** is written by libogc as one field but is really two:
cross-checked against Dolphin, bits 10:9 are the matrix *index* (off / 0 / 1 / 2) and bits 12:11
are the matrix *id* (indirect / S / T). libogc's `GX_ITM_*` constants are pre-composed so that a
single 4-bit write produces the right pair — see `GX_SetTevIndBumpST()` (`gx.c:4136-4162`), which
picks `GX_ITM_S0`/`GX_ITM_T0` style constants precisely to set the upper two bits.

### 11.2 Indirect matrices (`0x06`–`0x0E`)

Three registers per matrix, three matrices. From `GX_SetIndTexMatrix()` (`gx.c:4107-4130`), with
`idx = indtexmtx * 3` and `s = scale_exp + 17`:

```c
val = (_SHIFTL((0x06+idx),24,8)|_SHIFTL(s,22,2)|_SHIFTL(mb,11,11)|_SHIFTL(ma,0,11));
```

| Reg | Contents |
|---|---|
| `0x06 + 3m` | Matrix *m* column A |
| `0x07 + 3m` | Matrix *m* column B |
| `0x08 + 3m` | Matrix *m* column C |

so the nine registers are `0x06`–`0x0E` for m = 0, 1, 2.

| Bits | Field |
|---|---|
| 10:0 | Matrix element `offset_mtx[0][col]`, signed 11-bit, scaled by 1024 |
| 21:11 | Matrix element `offset_mtx[1][col]`, signed 11-bit, scaled by 1024 |
| 23:22 | Two bits of the shared 6-bit scale exponent: register A carries bits 1:0, register B bits 3:2 (`s>>2`), register C bits 5:4 (`s>>4`) |

The scale exponent stored is `scale_exp + 17`, spread two bits at a time across the three
registers.

Cross-checked against Dolphin: same two 11-bit signed elements and the same 2-bit scale fragment
at 23:22.

### 11.3 Indirect texture mask (`0x0F`)

Register number from `gx.c:1102`:
`__gx->tevIndMask = (__gx->tevIndMask&~0xff000000)|(_SHIFTL(0x0f,24,8));`

| Bits | Field |
|---|---|
| 7:0 | Bitmask of which texture maps are used by indirect stages — bit *t* set means texture map *t* is fed to the indirect unit |
| 23:8 | **Unverified:** not written. |

Computed in `__GX_UpdateBPMask()` (`gx.c:929-965`) as `nres |= (1<<ntexmap)` over the active
indirect stages. On Wii this recomputation is compiled out (`#if defined(HW_DOL)`), which is worth
knowing if comparing traces between the two machines.

### 11.4 Indirect texture coordinate scale (`0x25`, `0x26`)

Shadow array indices 0 and 1, so registers `0x25` and `0x26` (`gx.c:1138-1143`). From
`GX_SetIndTexCoordScale()` (`gx.c:4174-4197`):

| Reg | Bits | Field |
|---|---|---|
| `0x25` | 3:0 | Indirect stage 0 S scale exponent |
| `0x25` | 7:4 | Indirect stage 0 T scale exponent |
| `0x25` | 11:8 | Indirect stage 1 S scale exponent |
| `0x25` | 15:12 | Indirect stage 1 T scale exponent |
| `0x26` | 3:0 | Indirect stage 2 S scale |
| `0x26` | 7:4 | Indirect stage 2 T scale |
| `0x26` | 11:8 | Indirect stage 3 S scale |
| `0x26` | 15:12 | Indirect stage 3 T scale |
| both | 23:16 | **Unverified:** not written. |

Cross-checked against Dolphin: the scale is applied as 2^(−value).

### 11.5 Indirect texture order (`0x27`)

Shadow array index 2, so register `0x27`. From `GX_SetIndTexOrder()` (`gx.c:4548-4570`) and read
back in `__GX_SetSUTexRegs()` (`gx.c:880-897`):

| Bits | Field |
|---|---|
| 2:0 | Indirect stage 0 texture map |
| 5:3 | Indirect stage 0 texture coordinate |
| 8:6 | Indirect stage 1 texture map |
| 11:9 | Indirect stage 1 texture coordinate |
| 14:12 | Indirect stage 2 texture map |
| 17:15 | Indirect stage 2 texture coordinate |
| 20:18 | Indirect stage 3 texture map |
| 23:21 | Indirect stage 3 texture coordinate |

A clean 6-bits-per-stage packing, map before coordinate. Cross-checked against Dolphin: identical.

---

## 12. Miscellaneous rasteriser and PE registers

### 12.1 Field mask (`0x44`)

`GX_SetFieldMask()` (`gx.c:4428-4434`):

| Bits | Field |
|---|---|
| 0 | Write odd field |
| 1 | Write even field |
| 23:2 | **Unverified:** not written. |

### 12.2 Field mode (`0x68`)

`GX_SetFieldMode()` (`gx.c:4442`): `GX_LOAD_BP_REG(0x68000000|(field_mode&1));`

| Bits | Field |
|---|---|
| 0 | Field mode enable |
| 23:1 | **Unverified:** not written. |

Bracketed by texture-state flushes on both sides.

### 12.3 Line/point width (`0x22`)

Register number from `gx.c:1121`. Written by `GX_SetLineWidth()`, `GX_SetPointSize()` and
`GX_SetFieldMode()`.

| Bits | Field | Evidence |
|---|---|---|
| 7:0 | Line width, in 1/6 pixel units | `(__gx->lpWidth&~0xff)\|(width&0xff)` — `gx.c:3763` |
| 15:8 | Point size, in 1/6 pixel units | `(_SHIFTL(width,8,8))` — `gx.c:3770` |
| 18:16 | Line texture-offset format | `(_SHIFTL(fmt,16,3))` — `gx.c:3764` |
| 21:19 | Point texture-offset format | `(_SHIFTL(fmt,19,3))` — `gx.c:3771` |
| 22 | Adjust for interlaced aspect ratio (pixels of AR 1/2) | `(_SHIFTL(half_aspect_ratio,22,1))` — `gx.c:4438` |
| 23 | **Unverified:** not written. |

Cross-checked against Dolphin: identical.

### 12.4 Performance and misc registers

These are written by libogc but carry no rendering state; an emulator can usually ignore them.
Listed so a command-stream decoder does not mis-parse them.

| Reg | Name | Notes |
|---|---|---|
| `0x23` | Performance counter 0, triangle metric select | `GX_SetGPMetric()` (`gx.c:4865+`) writes literals such as `0x2300AE7F`, `0x2402C0C6`. `GX_ClearGPMetric()` writes `0x23000000`. Payload layout **Unverified** — libogc only ever writes opaque constants. |
| `0x24` | Performance counter 0, quad metric select | Same; cleared with `0x24000000`. |
| `0x67` | Performance counter 1 select | Cleared with `0x67000000`; ten distinct literals at `gx.c:4999-5026`. Payload layout **Unverified**. |
| `0x55`, `0x56` | Bounding-box clear | `GX_ClearBoundingBox()` writes `0x550003ff` and `0x560003ff` (`gx.c:2128-2129`) — a 10-bit field set to its maximum, resetting min/max accumulators. |
| `0x57` | Clear pixel performance counters | Not written anywhere in `gx.c`. **Unverified:** register number is from Dolphin's list only. |
| `0x58` | Revision bits | `GX_LOAD_BP_REG(0x5800000f);` in `__GX_InitRevBits()` (`gx.c:253`). Payload is the single constant `0x0f`; layout **Unverified**. |
| `0x46`, `0x69` | Bus clock counters 0 and 1 | Programmed once in `GX_Init()` from a divided timebase: `0x69000000\|((_SHIFTR(res,11,24))\|0x0400)` and `0x46000000\|(res\|0x0200)` (`gx.c:1148`, `gx.c:1153`). Layout **Unverified** beyond the visible flag bits `0x0400` / `0x0200`. |

### 12.5 Z texture (`0xF4`, `0xF5`)

`GX_SetZTexture()` (`gx.c:2777-2788`):

```c
val = (u32)(_SHIFTL(op,2,2))|(fmt&3);
GX_LOAD_BP_REG(0xF4000000|(bias&0x00FFFFFF));
GX_LOAD_BP_REG(0xF5000000|(val&0x00FFFFFF));
```

| Reg | Bits | Field |
|---|---|---|
| `0xF4` | 23:0 | Z texture bias, 24-bit |
| `0xF5` | 1:0 | Z texture format: 0 = 8-bit (`GX_TF_Z8`), 1 = 16-bit (`GX_TF_Z16`), 2 = 24-bit (everything else) |
| `0xF5` | 3:2 | Z texture operation (disable / add / replace) |
| `0xF5` | 23:4 | **Unverified:** not written. |

Cross-checked against Dolphin: same 2+2 split on `0xF5`.

### 12.6 BP write mask (`0xFE`)

`GX_SetCoPlanar()` (`gx.c:3937-3941`) emits `GX_LOAD_BP_REG(0xFE080000);` immediately before
re-writing genMode.

| Bits | Field |
|---|---|
| 23:0 | Bitmask applied to the **next** BP register write: only bits set here are updated in the destination register. |

Here `0x080000` is bit 19 — exactly the coplanar bit in genMode — so the pair means "update only
genMode bit 19". An emulator must implement this as a one-shot mask consumed by the next BP write;
ignoring it will cause spurious full-register updates of genMode.

**Unverified:** whether the mask persists beyond a single write, and whether hardware resets it to
all-ones afterwards. libogc uses it exactly once and never resets it, which suggests it is
self-clearing, but that is an inference, not evidence.

---

## 13. Fog

### 13.1 Fog parameters (`0xEE`–`0xF2`)

`GX_SetFog()` (`gx.c:4249-4319`). The fog equation uses three constants A, B and C; A and C are
transmitted as truncated IEEE-754 floats and B as an explicit mantissa/exponent pair.

**`0xEE` — parameter A:** `val = 0xee000000|(_SHIFTR(a_hex,12,20));` (`gx.c:4305`) where `a_hex`
is the raw IEEE-754 bit pattern of the float. Shifting right 12 and keeping 20 bits retains
sign + exponent + the top 11 mantissa bits.

| Bits | Field |
|---|---|
| 10:0 | Mantissa (top 11 bits of the IEEE-754 mantissa) |
| 18:11 | Exponent (IEEE-754 biased exponent) |
| 19 | Sign |
| 23:20 | **Unverified:** not written. |

**`0xEF` — B magnitude:** `val = 0xef000000|(b_m&0x00ffffff);` (`gx.c:4308`)

| Bits | Field |
|---|---|
| 23:0 | B mantissa as a 24-bit integer: `(u32)(B_mant * 8388638.0f)` where `B_mant` is normalised into [0.5, 1.0) |

**`0xF0` — B exponent:** `val = 0xf0000000|(b_expn&0x1f);` (`gx.c:4311`)

| Bits | Field |
|---|---|
| 4:0 | B's binary exponent, 5 bits |
| 23:5 | **Unverified:** not written. |

For orthographic fog libogc sets both B fields to zero (`gx.c:4265-4266`).

**`0xF1` — parameter C, projection and type:**
`val = 0xf1000000|(_SHIFTL(type,21,3))|(_SHIFTL(proj,20,1))|(_SHIFTR(c_hex,12,20));` (`gx.c:4314`)

| Bits | Field |
|---|---|
| 10:0 | C mantissa |
| 18:11 | C exponent |
| 19 | C sign |
| 20 | Projection: 0 = perspective, 1 = orthographic (derived as `_SHIFTR(type,3,1)`, `gx.c:4255`) |
| 23:21 | Fog function select (none / linear / exp / exp2 / backward-exp / backward-exp2) |

**`0xF2` — fog colour:** written both by `GX_SetFog()` (`gx.c:4317`) and `GX_SetFogColor()`
(`gx.c:4374`):

| Bits | Field |
|---|---|
| 7:0 | Blue |
| 15:8 | Green |
| 23:16 | Red |

No alpha. Note this is **B, G, R** order — the opposite of the TEV colour registers.

Cross-checked against Dolphin: same float-field split on `0xEE` and `0xF1`, same proj/fsel bits,
same BGR colour order.

### 13.2 Fog range adjust (`0xE8`–`0xED`)

`GX_SetFogRangeAdj()` (`gx.c:4348-4370`).

**`0xE8` — range base:** `val = 0xe8000000|(_SHIFTL(enable,10,1))|((center + 342)&0x03ff);`

| Bits | Field |
|---|---|
| 9:0 | Viewport centre X, biased by +342 (the same 0x156 bias used by the scissor) |
| 10 | Range adjustment enable |
| 23:11 | **Unverified:** not written. |

**`0xE9`–`0xED` — range table**, five registers holding ten 12-bit entries:

| Reg | Bits 11:0 | Bits 23:12 |
|---|---|---|
| `0xE9` | K0 | K1 |
| `0xEA` | K2 | K3 |
| `0xEB` | K4 | K5 |
| `0xEC` | K6 | K7 |
| `0xED` | K8 | K9 |

Each entry is computed as `(u32)(val6*256.0f) & 0x0fff` (`gx.c:4344-4345`) — an 8.4 fixed-point
range multiplier. libogc writes these five registers **only when `enable` is true**
(`gx.c:4351-4367`); register `0xE8` is written unconditionally.

Cross-checked against Dolphin: base register with a 10-bit centre and enable at bit 10, plus five
table registers.

---

## 14. Summary of what could not be determined

The following are recorded as **Unverified:** above and should be treated as unknown by an
emulator until hardware-tested:

1. **genMode bits 7 and 8.** libogc never writes them. Bit 8 may be flat shading; Dolphin marks it
   unconfirmed too.
2. **Copy trigger (`0x52`) bit 2.** Never written by libogc; unknown in Dolphin as well.
3. **Performance counter payloads (`0x23`, `0x24`, `0x67`).** libogc writes only opaque literal
   constants, one per metric. The register numbers are certain; the field layout is not.
4. **Revision bits (`0x58`) payload layout.** Only the single constant `0x0f` is ever written.
5. **Bus clock registers (`0x46`, `0x69`) payload layout.** Only the divided-timebase encoding and
   the flag bits `0x0200` / `0x0400` are visible.
6. **Clear-pixel-performance register (`0x57`).** Not written anywhere in `gx.c`; the register
   number comes from Dolphin's list alone and is not corroborated by libogc.
7. **Texture invalidate (`0x66`) field split.** The 9-bit line index and 4-bit size code rest on
   libogc alone; Dolphin's trimmed header treats the register as opaque.
8. **Destination alpha (`0x42`) bits 11:10.** The libogc evidence for the Y8/U8/V8 selector is
   direct, but Dolphin's header does not declare the field, so there is no second source.
9. **EFB copy source height field position.** libogc contradicts itself (writes at bits 19:10 via
   `XY()`, reads at bits 21:12 at `gx.c:1992`). The write path is almost certainly correct, but
   Dolphin's trimmed header could not settle it.
10. **BP write mask (`0xFE`) persistence.** Whether the mask applies to exactly one subsequent
    write or persists is inferred, not proven.
11. **Alpha compare logic-op enum ordering** (bits 23:22 of `0xF3`). The bit position is certain;
    the AND/OR/XOR/XNOR numbering comes from the GX API enum in `gx.h`, which was not read.
12. **Draw done (`0x45`) payloads other than 2.** libogc only ever writes 2.

Additionally, two libogc source-level quirks are documented above and should **not** be copied
into an emulator: the narrow scissor field shifts (§3.2) and the SU-size bit-16 wrap/range-bias
conflation (§8.5).
