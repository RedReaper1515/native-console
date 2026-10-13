# GameCube Texture Formats

## Provenance

This document is the texture-side companion to [`gc-bp-registers.md`](gc-bp-registers.md) and
follows the same rules.

The primary source is devkitPro/libogc's `gx.c` (zlib licence, 5199 lines, `scratchpad/gx.c`), the
GX implementation that shipped on real hardware. The `GX_TF_*` / `GX_CTF_*` / `GX_TL_*` numeric
values come from `D:\devkitPro\libogc\include\ogc\gx.h`. Dolphin's headers in
`roms/gc/hwtests/gxtest/` were **read as a cross-check only** — no code, comment text or wording
has been copied; "cross-checked" below is a statement about a fact, not a reuse of expression.

**What `gx.c` does and does not settle.** GX is a *submission* library: it builds registers and
hands the GPU a pointer. It therefore proves, directly and quotably:

* every format's numeric code, tile width and height in texels, and bytes per tile (§1, §2);
* the two-plane nature of `RGBA8` (§3.7);
* the address arithmetic — tile counts, the 32-byte tile granule, and the mipmap chain packing
  (§5);
* the TLUT's location, entry count encoding, and the three palette formats (§4);
* the wrap-mode and filter register encodings (§6).

It contains **no per-texel decode code at all** — there is not one expression in `gx.c` that
converts a stored texel to a colour. Neither do the trimmed Dolphin headers available here, which
carry register layouts but no decoder. So the bit-level layouts in §3 (which nibble, which byte,
how channels widen, the CMPR interpolation weights) are **not derivable from `gx.c`**. They were
originally stated with an explicit provenance note each, and the ones genuinely easy to get
backwards were marked **Unverified:** with the test that would settle them. §8 lists all of these
in one place, now annotated with their current status.

Line numbers refer to the copy of `gx.c` used for this analysis.

### Second pass: permissive cross-sources for the texel layer

The §3 rules have since been checked against independent, permissively-licensed implementations and
documentation. Dolphin was deliberately **not** used — it is GPL, and the point of this pass was to
obtain confirmation this MIT project may rely on. No code was copied from any source below; every
rule is restated in prose and notation here.

| Source | Licence | File / section used | What it settles |
|---|---|---|---|
| magcius/**noclip.website** | MIT (`LICENSE`; GitHub's detector reports "NOASSERTION" only because the boilerplate is lightly reworded) | `rust/src/gx_texture.rs` | every §3 texel rule, CMPR, palettes |
| encounter/**aurora** | MIT (Luke Street, 2022) | `lib/gfx/texture_convert.cpp` | same set, independently compiled against real GX titles |
| LagoLunatic/**gclib** | MIT | `gclib/texture_utils.py` | same set, plus an encoder for every format |
| **GRRLIB** | MIT | `GRRLIB/GRRLIB/GRRLIB_texEdit.c`, `RawTo4x4RGBA()` | RGBA8 tile layout and within-tile texel order, from a Wii homebrew library that feeds the result straight to real GX hardware |
| devkitPro/**libogc** `gx.h` | zlib (already the primary source) | doxygen note at `gx.h:4287-4288` | the power-of-two requirement for `GX_REPEAT` / `GX_MIRROR` |
| **Custom Mario Kart Wiiki**, "Image Formats" | community wiki; site terms permit redistribution and adaptation with attribution under a similar licence — i.e. CC-BY-SA-like, **not** permissive | whole page | corroboration only; facts extracted, no wording reused |
| **YAGCD** chapter 17 | community documentation, no licence stated | §17.14.1 CMPR | CMPR sub-tile order and endpoint comparison; the chapter is thin and self-flagged ("this section is screwed") for S3TC |
| impiaaa/**smstools** | **no licence declared** | `texture.py` | behaviour cross-check only, on the same footing as Dolphin |

Rejected for licence: devkitPro `gamecube-tools` / `gxtexconv` (GPL-2), riidefi `gctex` (GPL-2),
trashbyte `LibGC` (GPL-3), z64tools `zztexview` (GPL-3).

Two things came out of this pass that change the document rather than merely confirming it:

* **§3.4 IA8 was backwards.** Alpha is the **high** byte and intensity the low byte, not the other
  way round. The three MIT decoders (noclip.website, aurora, gclib) agree, as does the Wiiki page,
  and gclib's IA8 *encoder* packs it the same way. This also dissolves the
  "IA4 and IA8 disagree with each other" warning that used to lead §8 — both formats put alpha in
  the **high** half of their texel.
* **§3.11 CMPR's transparent index** is disputed between sources; see the note there.

A pattern worth knowing when weighing the sources: implementations written for *emulation accuracy*
(noclip, aurora) reproduce the GameCube's CMPR quirks, while implementations written as *content
tools* (gclib, and the two wikis) describe CMPR as plain DXT1 and inherit DXT1's exact-thirds
interpolation and black transparent texel. Where those two camps split, the accuracy camp is the
one to follow.

---

## 1. The format table

The sampleable texture formats. Tile dimensions and bytes per tile are read directly out of
`GX_InitTexObj()` (`gx.c:3255-3292`) and `GX_GetTexBufferSize()` (`gx.c:3073-3114`); the numeric
codes out of `gx.h:291-301`. Bits per texel is then forced by the identity
`bytes_per_tile * 8 == tile_w * tile_h * bpp`.

| Name (libogc) | Dolphin name | Value | Bits/texel | Tile (W×H texels) | Bytes/tile |
|---|---|---|---|---|---|
| `GX_TF_I4` | I4 | `0x0` | 4 | 8 × 8 | 32 |
| `GX_TF_I8` | I8 | `0x1` | 8 | 8 × 4 | 32 |
| `GX_TF_IA4` | IA4 | `0x2` | 8 | 8 × 4 | 32 |
| `GX_TF_IA8` | IA8 | `0x3` | 16 | 4 × 4 | 32 |
| `GX_TF_RGB565` | RGB565 | `0x4` | 16 | 4 × 4 | 32 |
| `GX_TF_RGB5A3` | RGB5A3 | `0x5` | 16 | 4 × 4 | 32 |
| `GX_TF_RGBA8` | RGBA8 | `0x6` | 32 | 4 × 4 | **64** (two 32-byte planes) |
| `GX_TF_CI4` | C4 | `0x8` | 4 | 8 × 8 | 32 |
| `GX_TF_CI8` | C8 | `0x9` | 8 | 8 × 4 | 32 |
| `GX_TF_CI14` | C14X2 | `0xA` | 16 | 4 × 4 | 32 |
| `GX_TF_CMPR` | CMPR | `0xE` | 4 (amortised) | 8 × 8 | 32 |

Codes `0x7`, `0xB`, `0xC`, `0xD` and `0xF` are undefined; Dolphin's name table leaves exactly
those five entries blank, which cross-checks the set.

**The 32-byte invariant.** Every format except `RGBA8` stores one tile in exactly 32 bytes — the
TMEM cache line. That is visible as the constant `bitsize` in `GX_GetTexBufferSize()`
(`gx.c:3113-3114`):

```c
bitsize = 32;
if(fmt==GX_TF_RGBA8 || fmt==GX_TF_Z24X8) bitsize = 64;
```

(the variable is misnamed — the value is **bytes per tile**, as `gx.c:3144` multiplies it by the
tile count to get a byte size). The same fact appears in the copy path as a *plane* count,
`gx.c:1888`:

```c
if(fmt==GX_TF_RGBA8 || fmt==GX_TF_Z24X8) *zplanes = 2;
```

So `RGBA8` is not a 64-byte tile format so much as a format whose tile is two ordinary 32-byte
tiles stacked (§3.7).

### 1.1 Copy-only and Z formats

These cannot be sampled as ordinary textures but appear as EFB-copy destinations and as Z
textures. `gx.c` gives their tile geometry in the same switches, so they are tabulated here for a
copy-engine implementation. `_GX_TF_CTF` is `0x20` and `_GX_TF_ZTF` is `0x10` (`gx.h:284-285`), so
these codes are the base code with those flag bits OR-ed in.

| Name | Value | Bits/texel | Tile (W×H) | Bytes/tile |
|---|---|---|---|---|
| `GX_CTF_R4` | `0x20` | 4 | 8 × 8 | 32 |
| `GX_CTF_RA4` | `0x22` | 8 | 8 × 8 (see §7.1) | 32 |
| `GX_CTF_RA8` | `0x23` | 16 | 4 × 4 | 32 |
| `GX_CTF_YUVA8` | `0x26` | — | — | — |
| `GX_CTF_A8` | `0x27` | 8 | 8 × 4 | 32 |
| `GX_CTF_R8` | `0x28` | 8 | 8 × 4 | 32 |
| `GX_CTF_G8` | `0x29` | 8 | 8 × 4 | 32 |
| `GX_CTF_B8` | `0x2A` | 8 | 8 × 4 | 32 |
| `GX_CTF_RG8` | `0x2B` | 16 | 4 × 4 | 32 |
| `GX_CTF_GB8` | `0x2C` | 16 | 4 × 4 | 32 |
| `GX_TF_Z8` | `0x11` | 8 | 8 × 4 | 32 |
| `GX_TF_Z16` | `0x13` | 16 | 4 × 4 | 32 |
| `GX_TF_Z24X8` | `0x16` | 32 | 4 × 4 | **64** (two planes) |
| `GX_CTF_Z4` | `0x30` | 4 | 8 × 8 | 32 |
| `GX_CTF_Z8M` | `0x39` | 8 | 8 × 4 | 32 |
| `GX_CTF_Z8L` | `0x3A` | 8 | 8 × 4 | 32 |
| `GX_CTF_Z16L` | `0x3C` | 16 | 4 × 4 | 32 |

**Unverified:** `GX_CTF_YUVA8` (`0x26`). libogc gives it no tile geometry in any of the three
switches (it falls into the `default` arm) and the only other place it appears is the copy-trigger
intensity bit (`gx.c:2114`). Its layout could not be determined.

*Still unverified after the permissive-source pass.* None of noclip.website, aurora, gclib or
GRRLIB implements it — every one of them handles only the eleven sampleable formats of §1. Neither
YAGCD chapter 17 nor the Custom Mario Kart Wiiki mentions a YUVA copy format at all; YAGCD's only
YUV material is the XFB's `YCbYCr` (§17.1), which is a framebuffer format with a different code and
no tile structure. The name is the only evidence there is.

`GX_TF_A8` is an alias for `GX_CTF_A8` (`gx.h:333`), not a separate format.

`GX_TF_CMPR` never appears in the copy-path switch (`__GetImageTileCount()`, `gx.c:1833-1888`),
which is consistent with CMPR not being a legal EFB-copy destination.

---

## 2. Tile geometry

libogc expresses tile dimensions as shift amounts, always in this shape (`gx.c:3294-3295`):

```c
nwd = ((wd+(1<<xshift))-1)>>xshift;
nht = ((ht+(1<<yshift))-1)>>yshift;
```

i.e. `xshift = log2(tile_width)` and `yshift = log2(tile_height)`, and the tile count is the
dimension rounded **up** to a whole tile. Three shift groups exist, and they map one-to-one onto
bits per texel:

| Group | xshift, yshift | Tile | Formats | bpp |
|---|---|---|---|---|
| A | 3, 3 | 8 × 8 | I4, C4, CMPR, CTF_R4, CTF_Z4 | 4 |
| B | 3, 2 | 8 × 4 | I8, IA4, C8, Z8, CTF_A8/R8/G8/B8/Z8M/Z8L | 8 |
| C | 2, 2 | 4 × 4 | IA8, RGB565, RGB5A3, RGBA8, C14X2, Z16, Z24X8, CTF_RA8/RG8/GB8/Z16L | 16 or 32 |

A texture object also records a 2-bit **tile type** used by the preload register (`0x63` bits
16:15, see the sibling document §8.7). From `GX_InitTexObj()`:

| Tile type | Formats | Evidence |
|---|---|---|
| 0 | CMPR | `gx.c:3285` |
| 1 | I4, C4 | `gx.c:3260` |
| 2 | I8, IA4, C8, IA8, RGB565, RGB5A3, RGBA8, and the `default` arm | `gx.c:3267, 3275, 3290` |
| 3 | C14X2 | `gx.c:3280` |

So the tile type is essentially "how a TMEM line is interpreted": compressed, 4-bit, plain, or
paletted-16-bit.

The texture object's cached tile count is `nwd * nht` truncated to 15 bits (`gx.c:3296`), which is
the value loaded into the preload count field.

---

## 3. Decoding one texel to RGBA8

**Byte order.** The GameCube is big-endian and no texture format is byte-swapped relative to the
machine: every 16-bit texel and every 16-bit palette entry is a big-endian halfword, and every
32-bit quantity is a big-endian word. (Provenance: platform. `gx.c` reads and writes textures only
through physical addresses and never touches texel bytes, so it cannot confirm this directly; all
known decoders assume it and all GC texture tooling produces it.)

**Channel widening.** Sub-8-bit channels are widened to 8 bits by *replicating the value's own top
bits into the low bits* — equivalently, multiply by `255 / (2^n - 1)` with the exact rounding the
replication gives:

| Source width | Expression | Check |
|---|---|---|
| 3 bits | `(v << 5) \| (v << 2) \| (v >> 1)` | `7 → 0xFF`, `0 → 0x00` |
| 4 bits | `(v << 4) \| v` | `0xF → 0xFF` |
| 5 bits | `(v << 3) \| (v >> 2)` | `0x1F → 0xFF` |
| 6 bits | `(v << 2) \| (v >> 4)` | `0x3F → 0xFF` |

**No GameCube texture format deviates from this rule** — not RGB565's green, not RGB5A3 in either
of its cases, not the 3-bit alpha.

*Settled.* All four expressions above, including the awkward 3-bit one, appear character-for-
character in noclip.website's `gx_texture.rs`, aurora's `texture_convert.cpp` and gclib's
`texture_utils.py` (all MIT), which between them cover every GameCube format and every channel
width. gclib additionally documents each as a bit-replication pattern (`00001234 -> 12341234` and
so on), and aurora and noclip both attribute the technique to the same well-known "expanding bits"
note. A strictly-correct `v * 255 / max` rounding differs from it by at most one LSB in the middle
of the range; no implementation uses that, so bit replication is what a hardware capture will
match.

**Texel order inside a tile is row-major**: texel index `k = row * tile_width + column`, rows top
to bottom.

*Settled.* The Custom Mario Kart Wiiki's "Image Formats" page states it directly — blocks are
stored in increasing order and the data within a block runs row by row, left to right, top to
bottom — and GRRLIB's `RawTo4x4RGBA()` (MIT) emits exactly that order when building a texture for
real GX hardware: an outer loop over the tile's four rows, an inner loop over its four columns,
written consecutively. The same order is implicit in every decoder consulted.

Throughout, `s` is the horizontal texel coordinate and `t` the vertical, both integers; `k` is the
within-tile index defined above, and `TB` is the tile's base byte offset from §5.

### 3.1 I4 (`0x0`) — 4 bpp, 8 × 8 tile

One byte holds two horizontally adjacent texels. **The high nibble is the left (even-`s`) texel**,
the low nibble the right one:

```c
u8  b = tex[TB + (k >> 1)];
u8  i4 = (k & 1) ? (b & 0x0F) : (b >> 4);
u8  i  = (i4 << 4) | i4;              /* 4 -> 8 */
rgba = (i, i, i, i);                  /* intensity replicated to R, G, B and A */
```

The intensity goes to **all four** channels including alpha — an I4/I8 texture is not opaque
white-on-black, it is a grey with matching alpha.

*Settled:* the nibble order is high-nibble-first. noclip.website's `gx_texture.rs`, aurora's
`texture_convert.cpp` and gclib's `texture_utils.py` (all MIT) each select the high nibble for the
even within-tile index and the low nibble for the odd one, in both I4 and C4, and gclib's encoder
writes them back the same way. §3.8 therefore inherits the same rule.

That the intensity also becomes alpha (rather than alpha being forced to 255) is confirmed by the
same three sources. The Custom Mario Kart Wiiki says alpha is 255 for I4 and I8, but it is
describing conversion to an editable image file, not what the texture unit presents to the TEV
stages; the three decoders agree against it.

### 3.2 I8 (`0x1`) — 8 bpp, 8 × 4 tile

```c
u8 i = tex[TB + k];
rgba = (i, i, i, i);
```

No widening, no ambiguity.

### 3.3 IA4 (`0x2`) — 8 bpp, 8 × 4 tile

One byte per texel, split into two nibbles:

```c
u8 b = tex[TB + k];
u8 a4 = b >> 4,  i4 = b & 0x0F;       /* high nibble = ALPHA, low = intensity */
u8 a = (a4 << 4) | a4;
u8 i = (i4 << 4) | i4;
rgba = (i, i, i, a);
```

*Settled:* alpha is the high nibble, intensity the low nibble. noclip.website, aurora and gclib
(all MIT) all decode it this way, gclib's encoder packs alpha back into the high nibble, and the
Custom Mario Kart Wiiki states it in words — the colour components come from the lower four bits
and the alpha from the upper four. The old caution here — that IA4 and IA8 use opposite orderings
— was a consequence of §3.4 being stated backwards; corrected, both formats put alpha in the high
half.

### 3.4 IA8 (`0x3`) — 16 bpp, 4 × 4 tile

One big-endian halfword per texel:

```c
u16 v = be16(tex + TB + k*2);
u8  a = v >> 8;                       /* high byte (first in memory) = ALPHA     */
u8  i = v & 0xFF;                     /* low  byte (second in memory) = intensity */
rgba = (i, i, i, a);
```

**Corrected — this rule used to be stated the other way round in this document.** *Settled:* alpha
is the **high** byte. Four independent sources agree and none dissents:

* noclip.website `rust/src/gx_texture.rs` (MIT) takes alpha from the first source byte and
  intensity from the second, in both the IA8 texel decoder and the `GX_TL_IA8` palette decoder;
* aurora `lib/gfx/texture_convert.cpp` (MIT) reads the texel as a host halfword *without* the
  byte-swap it applies to RGB565 and RGB5A3, then takes intensity from the top of that host value
  — which on a little-endian host is the second byte in memory, alpha the first;
* gclib `gclib/texture_utils.py` (MIT) reads a big-endian halfword and assigns the high byte to
  alpha and the low byte to the grey level, and its encoder packs them back the same way;
* the Custom Mario Kart Wiiki states it in words: the colour components take the lower byte and
  the alpha component the upper byte.

So IA4 and IA8 are **consistent** with each other — alpha occupies the high half of the texel in
both. The `GX_CTF_RA4` / `GX_CTF_RA8` comment text in `gx.h:308-309`, which lists red before
alpha, remains what it always looked like: a list of which channels are copied, not a layout.

### 3.5 RGB565 (`0x4`) — 16 bpp, 4 × 4 tile

```c
u16 v = be16(tex + TB + k*2);
u8 r5 = (v >> 11) & 0x1F;
u8 g6 = (v >>  5) & 0x3F;
u8 b5 =  v        & 0x1F;
rgba = ( (r5<<3)|(r5>>2), (g6<<2)|(g6>>4), (b5<<3)|(b5>>2), 0xFF );
```

Alpha is always 255 — RGB565 has no alpha channel and does not borrow one from anywhere.

### 3.6 RGB5A3 (`0x5`) — 16 bpp, 4 × 4 tile

A per-texel union of two layouts. **Bit 15 selects**: set means opaque, clear means the
alpha-bearing variant. Note the polarity — the "has alpha" case is the one with the bit *clear*.

**Case A — bit 15 = 1: RGB555, opaque.**

```c
u8 r5 = (v >> 10) & 0x1F;
u8 g5 = (v >>  5) & 0x1F;
u8 b5 =  v        & 0x1F;
rgba = ( (r5<<3)|(r5>>2), (g5<<3)|(g5>>2), (b5<<3)|(b5>>2), 0xFF );
```

All three channels are 5 bits, widened by the 5→8 rule. Alpha is forced to 255 (not to the value
of bit 15 or anything else).

**Case B — bit 15 = 0: A3RGB444.**

```c
u8 a3 = (v >> 12) & 0x07;             /* bits 14:12 */
u8 r4 = (v >>  8) & 0x0F;             /* bits 11:8  */
u8 g4 = (v >>  4) & 0x0F;             /* bits 7:4   */
u8 b4 =  v        & 0x0F;             /* bits 3:0   */
rgba = ( (r4<<4)|r4, (g4<<4)|g4, (b4<<4)|b4, (a3<<5)|(a3<<2)|(a3>>1) );
```

The three-bit alpha takes eight values, `0, 36, 73, 109, 146, 182, 219, 255`. Case B can therefore
never express full opacity *and* 5-bit colour at once, and case A can never express transparency —
which is the whole point of the format and the reason a3 = 7 must widen to exactly 255.

The two cases have different colour precision (5 bits vs 4), so a naive decoder that reads 5-bit
fields in both cases produces subtly wrong colours only on the transparent half of a texture.

### 3.7 RGBA8 (`0x6`) — 32 bpp, 4 × 4 tile, 64 bytes

The only format whose tile is not 32 bytes. The 64 bytes are **two independent 32-byte planes**,
each holding 16 two-byte pairs in the same row-major texel order:

* **Plane 0** — bytes `TB + 0 … TB + 31` — holds **alpha and red**: texel `k` at `TB + k*2`, alpha
  first, then red.
* **Plane 1** — bytes `TB + 32 … TB + 63` — holds **green and blue**: texel `k` at
  `TB + 32 + k*2`, green first, then blue.

```c
u8 a = tex[TB      + k*2 + 0];
u8 r = tex[TB      + k*2 + 1];
u8 g = tex[TB + 32 + k*2 + 0];
u8 b = tex[TB + 32 + k*2 + 1];
```

That the tile is two stacked 32-byte planes is direct from `gx.c`: the tile size is 64
(`gx.c:3113-3114`), the copy engine reports `*zplanes = 2` for exactly `RGBA8` and `Z24X8`
(`gx.c:1888`), the copy destination stride is `xtiles * zplanes` (`gx.c:2111`), and the mipmap
walker advances the *address* by twice the tile count while advancing both TMEM line counters by
one tile count each (`gx.c:3537-3540`):

```c
if(fmt==GX_TF_RGBA8) {
    tmem_even += tile_cnt;
    tmem_odd  += tile_cnt;
    maddr     += (tile_cnt<<1);
}
```

— i.e. the two planes of one level land in the even and odd TMEM banks respectively.

*Settled:* plane 0 holds alpha-then-red pairs and plane 1 green-then-blue, exactly as written
above. The decisive source is **GRRLIB**'s `RawTo4x4RGBA()` (`GRRLIB/GRRLIB/GRRLIB_texEdit.c`,
MIT) — an *encoder* in a Wii homebrew library that hands its output directly to GX as a
`GX_TF_RGBA8` texture, so its byte order is confirmed by the hardware every time the library
draws. It writes, per 4 × 4 block, sixteen alpha/red pairs (alpha first) and then sixteen
green/blue pairs (green first). gclib (MIT) reads alpha at `+k*2`, red at `+k*2+1`, green at
`+k*2+32` and blue at `+k*2+33`; aurora and noclip.website (MIT) both run two passes over the
block, the first writing alpha and red, the second green and blue. The Custom Mario Kart Wiiki
spells the 64 bytes out as sixteen `A R` pairs followed by sixteen `G B` pairs.

GRRLIB's bundled PNGU helper computes the same address as
`((tile_index << 5) + (row << 2) + col) << 1`, i.e. `tile_index*64 + k*2` with `k = row*4 + col`,
which is §5.1's arithmetic and §3's row-major order in one expression.

Note the AR / GB grouping is the same odd pairing the BP colour registers use (sibling document
§7.5 and §9.4), which is at least internally consistent with the rest of the GPU.

### 3.8 C4 (`0x8`) — 4 bpp index, 8 × 8 tile

Identical addressing to I4; the nibble is a **palette index 0..15**, not a colour:

```c
u8 b   = tex[TB + (k >> 1)];
u8 idx = (k & 1) ? (b & 0x0F) : (b >> 4);   /* high nibble = even s */
rgba   = palette_lookup(idx);               /* see §4 */
```

### 3.9 C8 (`0x9`) — 8 bpp index, 8 × 4 tile

```c
u8 idx = tex[TB + k];
rgba   = palette_lookup(idx);
```

### 3.10 C14X2 (`0xA`) — 16 bpp, 4 × 4 tile, 14-bit index

A big-endian halfword of which only the **low 14 bits** are the index; the top two bits are
unused:

```c
u16 v   = be16(tex + TB + k*2);
u16 idx = v & 0x3FFF;
rgba    = palette_lookup(idx);
```

The 14-bit width is corroborated by the TLUT size enumeration: the largest is `GX_TLUT_16K`,
defined as 1024 blocks of 16 entries (`gx.h:347` with the comment at `gx.h:338` that the units are
"number of 16 entry blocks") — exactly 16384 = 2^14 entries, which is precisely what a 14-bit
index addresses and no more.

*Settled (as far as documentation goes):* the two high bits are ignored. The Custom Mario Kart
Wiiki says so explicitly — "the top 2 bits of each value are ignored" — and noclip.website, aurora
and gclib (all MIT) each mask the halfword with `0x3FFF` before indexing. What no source states is
whether the hardware *masks* them or whether setting them is simply undefined, so an encoder
should still write them as zero.

### 3.11 CMPR (`0xE`) — 8 × 8 tile, 32 bytes, DXT1-derived

**Structure.** A 32-byte tile is **four independent 8-byte compression blocks**, each covering a
4 × 4 sub-block of the 8 × 8 tile. Within the tile the four blocks are in row-major order:

| Sub-block | Covers (within the 8×8 tile) | Byte range |
|---|---|---|
| 0 | s 0–3, t 0–3 | `TB + 0 … TB + 7` |
| 1 | s 4–7, t 0–3 | `TB + 8 … TB + 15` |
| 2 | s 0–3, t 4–7 | `TB + 16 … TB + 23` |
| 3 | s 4–7, t 4–7 | `TB + 24 … TB + 31` |

```c
u32 sub   = (((t & 4) >> 1) | ((s & 4) >> 2));   /* 0..3 as tabulated */
u32 block = TB + sub * 8;
```

so there are **two levels of tiling**: 8 × 8 tiles across the image, and 4 × 4 blocks within each
tile.

**Block contents.**

| Bytes | Contents |
|---|---|
| 0–1 | `color0`, a big-endian RGB565 value |
| 2–3 | `color1`, a big-endian RGB565 value |
| 4–7 | Sixteen 2-bit indices, one byte per row, row 0 first |

Within an index byte the texels run **left to right from the most significant bit pair**: texel
column 0 is bits 7:6, column 1 is bits 5:4, column 2 is bits 3:2, column 3 is bits 1:0.

```c
u16 c0 = be16(tex + block + 0), c1 = be16(tex + block + 2);
u8  row = tex[block + 4 + (t & 3)];
u8  idx = (row >> (6 - 2*(s & 3))) & 3;
```

**Mode selection.** The two endpoints are compared **as raw unsigned 16-bit packed values**, not
per channel and not after widening:

* `c0 > c1` → **four-colour mode**, every texel opaque.
* `c0 <= c1` → **three-colour-plus-transparent mode**.

Note the tie: equal endpoints select the transparent mode.

**Four-colour mode** (`c0 > c1`), per channel on the *widened 8-bit* values:

| Index | Colour | Alpha |
|---|---|---|
| 0 | `C0` | 255 |
| 1 | `C1` | 255 |
| 2 | `(5*C0 + 3*C1) >> 3` | 255 |
| 3 | `(3*C0 + 5*C1) >> 3` | 255 |

**Three-colour mode** (`c0 <= c1`):

| Index | Colour | Alpha |
|---|---|---|
| 0 | `C0` | 255 |
| 1 | `C1` | 255 |
| 2 | `(C0 + C1) >> 1` | 255 |
| 3 | `(C0 + C1) >> 1` — same colour as index 2 | **0** |

**Index 3's colour in this mode is the one genuinely disputed rule left in §3.** Two camps:

* noclip.website and aurora (both MIT) give index 3 the *midpoint* colour with alpha 0, and both
  annotate it as a deliberate GameCube difference from PC DXT1. smstools does the same.
* gclib (MIT), YAGCD §17.14.1 and the Custom Mario Kart Wiiki give index 3 fully transparent
  **black**, which is what the PC DXT1 specification says.

The table above follows the first camp, because the same split appears on the interpolation weights
below — where the first camp is demonstrably right about the hardware and the second camp is simply
inheriting DXT1. The distinction is invisible under ordinary alpha blending (the RGB is multiplied
by zero either way) and matters only where a texel's colour survives its alpha: bilinear filtering
across the block, an alpha test followed by a colour-only TEV stage, or a bit-exact capture
comparison. If a hardware test is ever run for §3.11, run it for this too.

**Where the GameCube differs from PC DXT1.** Three differences, all of which will produce a
plausible-looking but wrong image if missed:

1. **Endianness.** `color0` and `color1` are big-endian halfwords. PC DXT1 stores them
   little-endian.
2. **Index bit order.** The GameCube's first texel of a row is the **top** bit pair of the index
   byte. PC DXT1 puts the first texel in the **bottom** bit pair. This is a per-byte reversal, so a
   decoder that gets it wrong mirrors every 4 × 4 block horizontally — subtle enough to survive
   casual inspection.
3. **Interpolation weights.** The GameCube approximates the 1/3 and 2/3 interpolants with
   **3/8 and 5/8** (the `>> 3` above). PC DXT1 uses exact thirds:
   `(2*C0 + C1)/3` and `(C0 + 2*C1)/3`. The difference is at most a few LSBs per channel, so it
   never looks broken — it just never matches a hardware capture bit-for-bit.

   Worked example: `C0 = 255, C1 = 0`. PC gives 170 and 85; the GameCube gives 159 and 95.

Because the compression unit is the 8 × 8 tile, a CMPR mip level smaller than 8 × 8 still occupies
a full 32-byte tile — all four sub-blocks — with the surplus texels as padding (§5.2).

*Settled*, item by item. The 32-byte tile at 8 × 8 texels was already proven by `gx.c:3282-3285`
and `gx.c:3075-3078`; four 8-byte DXT1 blocks is the only structure that fits. The rest:

* **Sub-block ordering** — row-major 2 × 2 within the tile (upper-left, upper-right, lower-left,
  lower-right), as tabulated above. gclib (MIT) derives the sub-block's origin as
  `x = (i % 2) * 4`, `y = (i / 2) * 4`; noclip.website and aurora (MIT) iterate `yb` then `xb` in
  steps of 4; YAGCD §17.14.1 prints the same 2 × 2 grid numbered `0 1 / 2 3`, and the Custom Mario
  Kart Wiiki describes four 8-byte 4 × 4 sub-blocks per block.
* **Endpoint comparison** — an unsigned compare of the two raw packed 16-bit values, with the tie
  (`c0 == c1`) falling into the transparent mode. All four MIT/community sources take the
  `c0 > c1` branch for four-colour mode and the `else` for three-colour, so equality selects the
  transparent mode in every one of them. The Wiiki states it as a numeric comparison of the two
  `UInt16`s.
* **Index bit order** — texel 0 of a row is the **most significant** bit pair. gclib (MIT) reads
  the four index bytes as one big-endian 32-bit word and extracts texel `i` at bit position
  `(15 - i) * 2`, i.e. from the top down; noclip.website and aurora (MIT) take `(bits >> 6) & 3`
  and then shift the byte left by two per texel; smstools reverses the bit pairs of each index
  byte when converting a GameCube block to a PC DXT1 block, which is the same statement in
  encoder form.
* **Interpolation weights** — 3/8 and 5/8, as tabulated. noclip.website and aurora (MIT) both
  implement `(a*3 + b*5) >> 3` under a name that marks it as the S3TC blend, and both flag the
  three-colour branch as a GameCube difference. smstools uses the same weights.

  Dolphin was consulted here only as a behavioural reference, and only to break a tie: its
  maintainers changed Dolphin's CMPR decode *from* exact thirds *to* 3/8 and 5/8, on the reasoning
  that per-texel division is implausible in fixed-function hardware whereas two shifts and an add
  are not. Nothing from Dolphin is reproduced here.

  **Dissenting sources.** gclib (MIT), YAGCD §17.14.1 and the Custom Mario Kart Wiiki all give
  exact thirds. All three are content-authoring references that describe CMPR as "DXT1" and take
  DXT1's interpolation with it; YAGCD's chapter additionally disclaims its own S3TC section, and
  its three-colour case is visibly garbled (it gives index 3 a two-thirds blend *and* alpha 0).
  **Trust 3/8–5/8.**

A hardware capture would still be worth having, and the test in the original note is unchanged: a
single block with `C0 = 0xFFFF`, `C1 = 0x0000` and index 2 distinguishes 170 (thirds) from 159
(eighths) unambiguously.

---

## 4. Palettes (TLUT)

### 4.1 Which formats use one

`C4`, `C8` and `C14X2` — `gx.h` codes `0x8`, `0x9`, `0xA` — index a texture lookup table. libogc
treats exactly this contiguous range as "paletted" throughout, e.g. `gx.c:561`:

```c
if(fmt>=GX_TF_CI4 && fmt<=GX_TF_CI14)
```

and the GameCube region callback tests the same three codes numerically (`gx.c:582`). A paletted
texture must be created with `GX_InitTexObjCI()` (`gx.c:3301-3308`), which clears the texobj's
`0x02` "no TLUT" flag; `GX_LoadTexObjPreloaded()` then emits the per-unit TLUT register only when
that flag is clear (`gx.c:3467-3473`).

The index widths are 4, 8 and 14 bits respectively (§3.8–3.10), so the useful table sizes are 16,
256 and up to 16384 entries.

### 4.2 Where the table lives

The TLUT is **not** read from main memory at sample time. It is DMA'd into the upper half of TMEM
first, and the texture unit indexes it there.

* **Source.** BP register `0x64` carries the palette's physical address in main memory shifted
  right by 5 (`GX_InitTlutObj()`, `gx.c:3428`), so palettes are 32-byte aligned.
* **Destination.** BP register `0x65` carries a destination and a size. `GX_InitTlutRegion()`
  (`gx.c:3214-3224`) subtracts the TLUT base before encoding:

  ```c
  tmem_addr -= 0x80000;
  ptr->tmem_addr_conf = (ptr->tmem_addr_conf&~0x3ff)|(_SHIFTR(tmem_addr,9,10));
  ptr->tmem_addr_conf = (ptr->tmem_addr_conf&~0x1FFC00)|(_SHIFTL(tlut_sz,10,10));
  ```

  So **TLUTs live at TMEM `0x80000` and above**, addressed in 512-byte units, and the size field
  counts 16-entry blocks (`gx.h:338-348`: `GX_TLUT_16` = 1, `GX_TLUT_256` = 16, `GX_TLUT_16K` =
  1024).
* **Binding.** BP registers `0x98`–`0x9B` / `0xB8`–`0xBB` bind a loaded TLUT to a texture unit:
  bits 9:0 are the same 10-bit TMEM offset and bits 11:10 are the entry format (`gx.c:3661`, and
  the sibling document §8.6).

libogc's default layout (`gx.c:1190-1204`, and the `HW_DOL` branch at `gx.c:1217-1226`) carves out
16 regions of 256 entries at `0xC0000 + i*0x2000`, and 4 regions of 1K entries at
`0xE0000 + i*0x8000`. Those are a library convention, not hardware, but they are what a real
GameCube trace will show.

At 2 bytes per entry, a 16K-entry C14X2 palette is 32 KB and only the four large regions can hold
one.

### 4.3 Palette entry formats

Three, selected by bits 11:10 of the per-unit TLUT register, values from `gx.h:303-305`:

| Name | Value | Entry |
|---|---|---|
| `GX_TL_IA8` | `0x0` | 16-bit, decoded exactly as §3.4 |
| `GX_TL_RGB565` | `0x1` | 16-bit, decoded exactly as §3.5 |
| `GX_TL_RGB5A3` | `0x2` | 16-bit, decoded exactly as §3.6 |

Cross-checked against Dolphin: same three names at the same three values, and no fourth.

**Every palette entry is 16 bits**, whatever the format — there is no 32-bit palette. So:

```c
u16 entry = be16(tlut + tlut_base + idx*2);
rgba      = decode_16bpp(entry, tlut_format);   /* §3.4 / §3.5 / §3.6 */
```

Value `0x3` of the format field is undefined; **Unverified:** its behaviour. *Still unverified.*
The Custom Mario Kart Wiiki's palette table lists the same three values and no fourth, and
noclip.website, aurora and gclib each enumerate exactly three palette formats and treat anything
else as an error. No permissive source describes what the hardware does when the field is `0x3`;
an emulator should pick a defined behaviour (decoding it as one of the three, or as opaque black)
rather than assert, and flag it if it is ever seen.

A consequence worth stating: a `C8` + `RGB5A3` texture reaches full RGBA at 8 bits per texel, and
is the cheapest format on the machine that can express per-texel alpha at 4-bit colour precision.

---

## 5. Address arithmetic

### 5.1 Texel address within a level

Given a level's width `W` and height `H` in texels, a format's tile dimensions `TW` × `TH` and its
bytes-per-tile `TS` (§1), the byte offset of texel `(s, t)` from the level's base is:

```
tiles_per_row = ceil(W / TW)                        /* = (W + TW - 1) / TW  */
tile_index    = (t / TH) * tiles_per_row + (s / TW)
TB            = tile_index * TS                     /* the tile's base offset */
row           = t % TH
col           = s % TW
k             = row * TW + col                      /* within-tile texel index */
```

and then the within-tile offset depends only on bits per texel:

| bpp | Byte offset of texel `k` | Sub-byte selection |
|---|---|---|
| 4 | `TB + (k >> 1)` | high nibble if `k` even |
| 8 | `TB + k` | — |
| 16 | `TB + k*2` | big-endian halfword |
| 32 (RGBA8) | `TB + k*2` and `TB + 32 + k*2` | see §3.7 |
| CMPR | `TB + sub*8` | see §3.11 |

`tiles_per_row` uses the **ceiling**, and the row stride is therefore
`ceil(W / TW) * TS` bytes — a texture whose width is not a multiple of the tile width is padded
out to whole tiles, and the padding texels occupy real bytes. The ceiling is quoted directly from
`gx.c:3139` (and identically at `gx.c:3294`):

```c
xtiles = (wd+((1<<xshift)-1))>>xshift;
```

and the total size of a level is `xtiles * ytiles * TS` (`gx.c:3144`, with `bitsize` being bytes
per tile as noted in §1):

```c
size = ((xtiles*ytiles)*bitsize);
```

**This is the same tiling shape the EFB copy writes.** The copy path computes its tile counts with
the identical shift table and identical ceiling in `__GetImageTileCount()` (`gx.c:1833-1888`), and
its destination stride register (`0x4D`) is `xtiles * zplanes` (`gx.c:2111`). An EFB copy is
therefore *exactly* a texture of the destination format written at the origin of the destination
buffer, and a single address routine serves both.

### 5.2 Mipmaps

Levels follow the base level **immediately and contiguously**, with no alignment padding beyond
each level's own rounding up to whole tiles. Each level is half the previous level's width and
height, clamped to a minimum of 1.

The evidence is the mip-chain walker in `GX_PreloadEntireTexture()` (`gx.c:3531-3597`), which
advances the main-memory address by exactly the previous level's tile count (in 32-byte units,
since `maddr` is an address shifted right by 5) and then recomputes the tile count for the next
level (`gx.c:3542` and `gx.c:3594`):

```c
maddr += tile_cnt;
...
tile_cnt = (((w+(1<<xshift))-1)>>xshift)*(((h+(1<<yshift))-1)>>yshift);
```

with `RGBA8` advancing by `tile_cnt<<1` instead (`gx.c:3540`) because its tiles are 64 bytes.

So, with `level_size(w, h) = ceil(w/TW) * ceil(h/TH) * TS`:

```
offset(0) = 0
offset(n) = offset(n-1) + level_size(max(1, W >> (n-1)), max(1, H >> (n-1)))
```

The clamp to 1 is explicit in libogc's two mip loops (`gx.c:3582-3583`,
and `gx.c:3128-3131` in `GX_GetTexBufferSize()`).

Two consequences of the ceiling:

* A level smaller than one tile still costs a whole tile — a 1 × 1 level of a 4 × 4-tile format
  occupies 32 bytes, and a 1 × 1 CMPR level occupies 32 bytes as well.
* The chain does not shrink geometrically at the tail; the last several levels all cost `TS`
  bytes each.

The number of levels is bounded by the LOD registers (setmode1, sibling document §8.2), and
libogc derives a full chain length as `31 - cntlzw(max(W, H))` (`gx.c:3517-3520`), i.e.
`floor(log2(max(W, H)))` — the count of levels *after* the base.

`GX_GetTexBufferSize()` (`gx.c:3069`) is the library's answer to "how many bytes does this
texture need"; note it takes `maxlod` as a **count of levels**, not a maximum index, and see §7.2
for the bugs in that function.

---

## 6. Wrap modes and filters

### 6.1 Register encoding

All of this lives in texture setmode0 (`0x80`–`0x83`, `0xA0`–`0xA3`), documented as a register in
the sibling document §8.1. Repeated here in decode terms.

| Bits | Field | Values |
|---|---|---|
| 1:0 | Wrap S | 0 = clamp, 1 = repeat, 2 = mirror, 3 = undefined |
| 3:2 | Wrap T | same |
| 4 | Magnification filter | 0 = point, 1 = linear |
| 6:5 | Mip filter | 0 = none, 1 = point (discrete), 2 = linear (trilinear blend) |
| 7 | Minification filter | 0 = point, 1 = linear |

The wrap values are from `gx.h:420-422` (`GX_CLAMP` 0, `GX_REPEAT` 1, `GX_MIRROR` 2) and are
written through unmodified (`gx.c:3236-3237`):

```c
ptr->tex_filt = (ptr->tex_filt&~0x03)|(wrap_s&3);
ptr->tex_filt = (ptr->tex_filt&~0x0c)|(_SHIFTL(wrap_t,2,2));
```

so the API enum *is* the hardware encoding for wrap. Cross-checked against Dolphin: same three
values in the same order.

**Value 3 is not a legal wrap mode** (`gx.h:423` names it `GX_MAXTEXWRAPMODE`, a bound rather than
a mode). Cross-checked: Dolphin records that hardware testing shows value 3 behaving as clamp.
An emulator should do the same rather than assert.

The filter field is *not* a straight pass-through: libogc maps the six API filter modes through
`static const u8 _gx2HWFiltConv[] = {0x00,0x04,0x01,0x05,0x02,0x06,0x00,0x00};` (`gx.c:108`) into
the 3-bit field at bits 7:5 (`gx.c:3319`). Reading that table against `gx.h:1000-1005`:

| API mode | Table value | → min filter (bit 7) | → mip mode (bits 6:5) |
|---|---|---|---|
| `GX_NEAR` (0) | 0x00 | point | none |
| `GX_LINEAR` (1) | 0x04 | linear | none |
| `GX_NEAR_MIP_NEAR` (2) | 0x01 | point | point |
| `GX_LIN_MIP_NEAR` (3) | 0x05 | linear | point |
| `GX_NEAR_MIP_LIN` (4) | 0x02 | point | linear |
| `GX_LIN_MIP_LIN` (5) | 0x06 | linear | linear |

The magnification filter is a separate single bit and is read back as such (`gx.c:3046`):

```c
*magfilt = ptr->tex_filt & 0x10 ? GX_LINEAR : GX_NEAR;
```

Anisotropy (bits 20:19) takes 0, 1, 2 for 1×, 2×, 4× (`gx.h:1011-1013`); cross-checked against
Dolphin, same three values.

### 6.2 Resolving an out-of-range coordinate

Wrapping happens on the **texel** coordinate, after the normalised coordinate has been scaled by
the SU size registers (`0x30`–`0x3F`, sibling document §8.5, which hold width−1 and height−1).
With `size` the level's dimension in texels and `i` the integer texel coordinate before wrapping —
which may be negative or ≥ `size` — the resolved coordinate is:

| Mode | Resolution |
|---|---|
| Clamp (0) | `min(max(i, 0), size - 1)` |
| Repeat (1) | `i & (size - 1)` — equivalently `i mod size` with a non-negative result |
| Mirror (2) | `m = i & (size - 1)`; if `i & size` is set, use `size - 1 - m`, else `m` |
| 3 | behaves as clamp |

So a coordinate of 1.25 on a 64-texel axis (texel 80) resolves to texel 63 under clamp, texel 16
under repeat, and texel 47 under mirror; a coordinate of −0.25 (texel −16) resolves to 0, 48 and
15 respectively.

**Repeat and mirror require power-of-two dimensions.** The hardware implements both with bit
masking, not division — for repeat the low `log2(size)` bits, for mirror the same plus the next
bit up as the flip flag. A non-power-of-two texture with repeat set does not wrap at its own
width; it wraps at the next power of two and reads the tile padding.

*The constraint itself is now sourced.* libogc's own `gx.h` states it in the `GX_InitTexObj()`
doxygen block (`gx.h:4287-4288`, zlib): non-mipmapped textures need not be a power of two, but to
use `GX_REPEAT` or `GX_MIRROR` for `wrap_s` or `wrap_t` the width or height respectively must be.
That is a documented API precondition, which is consistent with a mask-based implementation and
rules out a true modulo, but it is a prohibition rather than a description.

**Unverified:** the exact behaviour of repeat or mirror on a non-power-of-two texture. No
permissive source describes it, and the decoders consulted for §3 are file-format tools with no
sampler at all, so they have nothing to say about wrapping. The masking model above is the
document's own inference and should be treated as such.

Mirror's period is `2 * size`: the texture runs forwards over `[0, size)` and backwards over
`[size, 2*size)`, so the mirrored copy is adjacent with no repeated edge texel.

Bit 16 of the SU size registers is worth re-reading in the sibling document (§8.5): libogc writes
the low bit of the wrap mode there, while the hardware field is the range-bias flag. Do not treat
it as a wrap mode.

---

## 7. libogc quirks not to copy

Three places in `gx.c` disagree with each other about tile geometry. The 32-byte tile invariant
(§1) resolves every one of them, but a reader who takes any single switch as gospel will get a
different answer, so all three are recorded.

### 7.1 IA4's tile height

| Function | IA4 tile | Line |
|---|---|---|
| `GX_InitTexObj()` | 8 × 4 | `gx.c:3262-3267` |
| `GX_GetTexBufferSize()` | 8 × 4 | `gx.c:3082-3095` |
| `__GetImageTileCount()` (copy path) | 8 × 8 | `gx.c:1838-1845` |
| `GX_PreloadEntireTexture()` mip loop | 8 × 8 | `gx.c:3557-3562` |

IA4 is 8 bits per texel, so 8 × 4 is the only geometry that fills a 32-byte line; **8 × 4 is
correct**. The two 8 × 8 sites over-count tiles by 2×, which for the copy path means an
over-large destination stride (register `0x4D`) whenever an EFB copy targets IA4, and for the
preload path a wrong tile count for IA4 mip levels. The same `GX_CTF_RA4` grouping in
`__GetImageTileCount()` is presumably the same mistake, which is why §1.1 flags `GX_CTF_RA4`'s
tile as "see §7.1"; RA4 is likewise 8 bpp and should be 8 × 4.

**Unverified:** whether the copy hardware actually accepts the doubled stride without visible
damage — libogc has evidently shipped this for years, which suggests IA4 EFB copies are rare
rather than that the stride is right.

*Still unverified.* This is a question about the EFB copy engine's response to a wrong register
value, not about a texel layout, so none of the permissive texture sources can reach it: they all
read finished texture files and never program the copy path. The 8 × 4 geometry itself is
independently confirmed — the Custom Mario Kart Wiiki's format table lists IA4 as 8 × 4 at 8 bits
per texel, and gclib, aurora and noclip.website all use 8 × 4 — so §7.1's conclusion that libogc's
two 8 × 8 sites are bugs stands on more than the 32-byte invariant now. Only the consequence for
register `0x4D` remains open.

### 7.2 `GX_GetTexBufferSize()` and the paletted formats

Its format switch (`gx.c:3073-3111`) lists `GX_TF_I4`, `CMPR` and the copy formats but **never
lists `GX_TF_CI4`, `CI8` or `CI14`**, so all three fall into the `default` arm and are sized as
4 × 4 tiles. The correct geometry (from `GX_InitTexObj()`, which does list them) is 8 × 8, 8 × 4
and 4 × 4. The result is an over-estimate — 4× too large for C4, 2× for C8, correct for C14X2 —
so allocations made with it are safe but wasteful, and the function must not be used to compute
mip-level *offsets* for paletted textures. Use §5 instead.

The same function's mipmap loop contains dead code (`gx.c:3124`):

```c
if(cnt==0) return size;
```

inside a `while(cnt)`, which can never be true.

### 7.3 The mip walker's height

`GX_PreloadEntireTexture()` computes both next-level dimensions from the width (`gx.c:3554-3555`):

```c
w = wd>>(i+1);
h = wd>>(i+1);
```

The second line should read `ht`. For a square texture it is harmless; for a non-square one the
preloaded tile counts for mip levels are wrong. Implement the chain per §5.2, from both
dimensions.

---

## 8. Status of the thirteen open questions

`gx.c` contains no texel-decode code, so everything in §3 below the tile level rested originally on
assumed hardware behaviour. A second pass against permissively-licensed decoders, encoders and
community documentation (see the provenance header) closed most of it. Nine items are settled, one
is half-settled, and three remain open.

| # | Item | Status |
|---|---|---|
| 1 | IA4 / IA8 channel order (§3.3, §3.4) | **Settled — and IA8 was corrected.** Alpha is the high half of the texel in *both* formats: high nibble in IA4, high byte in IA8. noclip.website, aurora, gclib (MIT) and the Custom Mario Kart Wiiki all agree. The "the two formats are inconsistent" warning is withdrawn. |
| 2 | CMPR interpolation weights (§3.11) | **Settled: 3/8 and 5/8.** noclip.website, aurora (MIT) and smstools. Content tools (gclib, YAGCD, the Wiiki) say exact thirds; they are inheriting the PC DXT1 specification. |
| 3 | CMPR index bit order (§3.11) | **Settled: most significant bit pair first.** gclib, noclip.website, aurora (MIT), and smstools' GameCube→DXT1 bit-pair reversal. |
| 4 | CMPR sub-block order and endpoint comparison (§3.11) | **Settled.** Row-major 2 × 2 sub-blocks; unsigned compare of the packed 16-bit endpoints; the `c0 == c1` tie selects the transparent mode. Four sources plus YAGCD's grid diagram. |
| 5 | RGBA8 plane assignment (§3.7) | **Settled: plane 0 = alpha, red; plane 1 = green, blue.** Confirmed by GRRLIB's on-hardware *encoder* (MIT) as well as three decoders and the Wiiki's byte diagram. |
| 6 | Nibble order for the 4-bit formats (§3.1, §3.8) | **Settled: high nibble first.** noclip.website, aurora, gclib (MIT), decode and encode alike. |
| 7 | Row-major texel order within a tile (§3) | **Settled.** Stated outright by the Wiiki; produced by GRRLIB's encoder; implicit in every decoder. |
| 8 | Channel-widening rule (§3) | **Settled: bit replication.** All four expressions, including the 3-bit one, are identical in noclip.website, aurora and gclib (MIT). |
| 9 | `GX_CTF_YUVA8` (`0x26`) (§1.1) | **Open.** No permissive source implements or documents it; none of the four decoders handles anything beyond the eleven sampleable formats, and neither wiki mentions it. |
| 10 | C14X2's two unused high bits (§3.10) | **Settled as far as documentation goes: ignored.** The Wiiki says so; all three decoders mask with `0x3FFF`. Whether the hardware masks or leaves it undefined is untested, so encoders should still write zero. |
| 11 | TLUT format value `0x3` (§4.3) | **Open.** Every source enumerates exactly three palette formats and describes no fourth. |
| 12 | Repeat and mirror on non-power-of-two textures (§6.2) | **Half-settled.** libogc's own `gx.h:4287-4288` (zlib) documents that `GX_REPEAT` and `GX_MIRROR` *require* power-of-two dimensions — consistent with masking, and it rules out a true modulo. What the hardware actually does when the rule is broken is still undescribed anywhere. |
| 13 | The doubled IA4 copy stride (§7.1) | **Open.** A question about the copy engine's response to a bad register value; the texture sources have no copy path. IA4's 8 × 4 geometry is now independently confirmed, so §7.1's diagnosis is firmer, but the consequence is not. |

**One rule changed as a result of this pass** — §3.4 IA8 — and one previously-flat statement is now
marked as disputed: the colour (not the alpha) of CMPR index 3 in three-colour mode, where the
accuracy-oriented sources give the midpoint and the content tools give black. See §3.11.

Everything in §1, §2, §4, §5, §6.1 and §7 is derived directly from quoted `gx.c` / `gx.h`
expressions and is safe to implement as written.

**What a hardware test is still worth.** With items 1–8 confirmed by independent implementations,
the value of a capture has shifted: it is no longer about the basic layouts but about the three
remaining unknowns (9, 11, 13), the exact non-power-of-two wrap behaviour (12), and the one
disputed CMPR sub-rule. A single CMPR block with `C0 = 0xFFFF`, `C1 = 0x0000` sampled at indices 2
and 3 in both modes would settle the last of those.
