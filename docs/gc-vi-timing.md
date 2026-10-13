# GameCube Video Interface: Timing, Fields and Scaling

This is the companion to [`gc-video-out.md`](gc-video-out.md). That document establishes the VI
register map (offsets, field positions, addressing), the XFB's YUV 4:2:2 format, and everything
about the EFB→XFB copy. **None of that is repeated here.** This document covers what it leaves
open: what the timing numbers *mean*, how they add up, what makes one field different from the
other, what the raster-position counter actually counts, and what scaling the interface does and
does not do on the way out.

The division of labour is worth stating once, because it is the thing most easily got wrong:

* The **copy** (BP `0x49`–`0x54`) decides what is in the XFB — resolution, deflicker filtering,
  vertical rescale, YUV conversion. It runs once per frame, at the game's request.
* The **interface** (`0xCC002000`) decides *when* and *in what order* the XFB is read back out. It
  runs continuously, on its own clock, and the game only observes it through a position counter and
  four interrupts.

Almost every vertical operation lives in the first. That is the single most important conclusion of
§6.

## Provenance

Three sources, read directly for this document:

1. **devkitPro/libogc `video.c`** (zlib), the same 2986-line copy `gc-video-out.md` used
   (`scratchpad/video.c`); all line numbers below refer to it. This is the authority for the
   timing tables and for every register value quoted.
2. **libogc headers** `D:\devkitPro\libogc\include\ogc\{video.h, video_types.h}` (zlib).
3. **Our own runtime**: `runtime-gc/src/gc_io.c`, `runtime-gc/src/gc.c`,
   `runtime-gc/include/gc.h`.

No GPL source was consulted for this document — no Dolphin video-interface source is present in
this tree, and `roms/gc/hwtests/` contains nothing that exercises the VI. Every claim therefore
rests on **libogc's tables**, on **arithmetic that has to be true** given those tables, or on
**our runtime's code**, and each claim below says which. Claims that rest on none of the three are
marked **Unverified:** with the evidence that would settle them.

Arithmetic in this document was computed rather than hand-derived, and the identities in §2 and §3
were checked against **all eight** of libogc's timing-table entries, not just NTSC.

---

## 1. The clock, and the unit every horizontal number is in

`gc.h:65-79` states the core relation and it is correct:

```
27 MHz VI clock; 1716 VI clocks per NTSC line; 262.5 lines per field
  => 450,450 VI clocks per field
  => x 18 core cycles per VI clock = 8,108,100 core cycles per field
  => 486 MHz / 8,108,100 = 59.94 fields per second
```

But **the VI's horizontal registers are not in 27 MHz clocks.** They are in units of *half* a VI
clock period — i.e. 13.5 MHz for a 27 MHz VICLK, 27 MHz for the 54 MHz progressive VICLK. Call
this the **VI pixel**, because it is exactly one pixel of the 720-wide display window.

**Evidence (libogc tables + arithmetic that has to be true), four independent confirmations:**

| Observation | Source | At 13.5 MHz | At 27 MHz |
|---|---|---|---|
| `hlw` = 429 is a *half*-line, and an NTSC half-line is 31.78 µs | `video.c:1530` | 429 units ✔ | 858 units ✘ |
| `hsy` = 64 is the h-sync pulse; NTSC h-sync is 4.7 µs | `video.c:1531` | 63.5 ✔ | 127 ✘ |
| `hcs`=71 … `hce`=105 is the colour burst; the NTSC burst is 9 cycles of 3.579545 MHz = 2.51 µs | `video.c:1531` | 34 units = 2.52 µs ✔ | 1.26 µs ✘ |
| PAL: `hcs`=75 … `hce`=106; the PAL burst is 10 cycles of 4.43 MHz = 2.26 µs | `video.c:1549` | 31 units = 2.30 µs ✔ | 1.15 µs ✘ |

So a full NTSC line is `2 × hlw = 858` VI pixels = 1716 VI clocks, and a full PAL line is
`2 × 432 = 864` VI pixels = 1728 VI clocks. Both are the standard 858/864-sample lines of ITU-R
BT.601, which is where the 13.5 MHz comes from in the first place.

**Correction to `gc-video-out.md` §B.3.** That section's sanity check reads "`hlw × 2 = 858` VI
clocks per line, `858 × 525 = 450450` clocks per frame". The product is right and the labels are
not: 858 is VI *clocks per half-line* (equivalently 429 VI *pixels*), 525 is *half-lines per
field*, and 450450 is clocks per **field**, not per frame — which is why multiplying by 59.94, the
field rate, recovers 27 MHz. Nothing downstream of that section depends on the wrong labels, but
anyone deriving `hbs640`/`hbe640` from them will get §2's identity wrong.

### Why the progressive modes reuse `hlw = 429`

The progressive timing entry (`video.c:1579-1586`) has the same `hlw` as interlaced NTSC. It is not
a copy-paste: a 480p line is 31.78 µs, half an NTSC line, and the progressive VICLK is 54 MHz, so
the VI pixel halves too. 429 VI pixels is a half-line in both cases. **The unit is always
VICLK/2**, which is why the same number works for both and why VICLK (`0x6C`) and the horizontal
registers must be read together.

---

## 2. Horizontal timing: HTR0 and HTR1

`gc-video-out.md` §B.1 gives the field positions. What it does not give is what the values mean,
and the meaning is not the obvious one.

`__setHorizontalRegs()` (`video.c:1951-1965`):

```c
val1 = (tm->hbe640+dispPosX-40)&0x01ff;                  // -> HBE
val2 = (tm->hbs640+dispPosX+40)-(720-dispSizeX);         // -> HBS
```

For NTSC with the stock 640-wide centred window (`dispPosX = 40`, `dispSizeX = 640`) this gives
HBE = 162 and HBS = 373. Those are 211 apart, which is neither 640 nor 720 — so **HBE and HBS are
not two points on the same line.**

**HBE is an offset into the first half-line; HBS is an offset into the second half-line.** The
active picture therefore spans

```
width = hlw + HBS - HBE
```

which for NTSC is `429 + 373 - 162 = 640`, and for PAL is `432 + 380 - 172 = 640`. Both exact.

That is arithmetic that has to be true, and it is confirmed in general by substituting libogc's own
expressions:

```
width = hlw + val2 - val1
      = hlw + hbs640 - hbe640 + 80 - 720 + dispSizeX
      = dispSizeX        (because hlw + hbs640 - hbe640 = 640 for every timing entry)
```

The `+80 - 720` cancels exactly against the constant, for every mode, so the programmed active
width is always precisely `dispSizeX` and the horizontal pan `dispPosX` shifts both edges together
without changing it. The table names `hbe640`/`hbs640` literally mean "the HBE/HBS that yield a
640-pixel-wide window"; the arithmetic re-centres them for any other width.

`__VIInit()` (`video.c:2111-2112`) writes the table values raw — `_viReg[4] = hbs640<<1`,
`_viReg[5] = (hbe640<<7)|hsy` — which is exactly the `dispPosX = 40, dispSizeX = 640` case, so the
console powers up with a 640-wide window before any `VIDEO_Configure()`.

**Unverified:** whether the hardware counts HBE from the start of the *first* half-line of a
line-pair or from the start of every half-line, i.e. whether the "first/second half-line" framing
above is the hardware's model or merely an encoding that happens to produce the right width. The
width identity holds either way. A guest that programs HBE > hlw and looks for the resulting left
edge would settle it.

---

## 3. Vertical timing: VTR, VTO, VTE, and the accounting identity

### The units

| Register | Field | Unit |
|---|---|---|
| VTR (`0x00`) | EQU, bits 3:0 | **half-lines**, and the hardware uses *three* of them (see below) |
| VTR (`0x00`) | ACV, bits 13:4 | **whole lines** — the only vertical field that is not in half-lines |
| VTO (`0x0C`) | PRB, bits 9:0 | **half-lines**, odd field |
| VTO (`0x0C`) | PSB, bits 25:16 | **half-lines**, odd field |
| VTE (`0x10`) | PRB / PSB | **half-lines**, even field |

### The identity

> **`nhlines = 3·EQU + PRB + 2·ACV + PSB`**, separately for the odd and the even field.

`nhlines` is the timing table's half-lines-per-field figure; it is never written to a register —
libogc keeps it only to place the display interrupts (§7) and to interpret the position counter
(§5). The identity holds for **all eight** timing entries and for **both** fields of each, sixteen
checks in total, with no slack.

The `3·EQU` term is not a guess. `__VIDisplayPositionToXY()` (`video.c:2383-2451`) converts a
raster position to a picture line by subtracting `currTiming->equ*3` and then the field's PRB —
so the region the VI puts before pre-blanking is exactly three times the EQU field, in half-lines.
That is the vertical-sync block: for NTSC, `3 × 6 = 18` half-lines = 9 lines of equalising and
serration pulses, which is what NTSC specifies.

**Attribution:** the structure (`3·EQU`) rests on libogc's own conversion code; the identity itself
is arithmetic that has to be true and was checked exhaustively.

### The programmed values, per mode

`__setVerticalRegs()` (`video.c:1968-2018`) is the only writer. It branches on the **equalisation
count**, not on any mode flag:

```c
div1 = 2; div2 = 1;
if(equ>=10) { div1 = 1; div2 = 2; }
```

`equ ≥ 10` is true for exactly the two progressive entries (12 and 10) and false for the six
interlaced/double-strike ones (6, 6, 5, 5, 6, 6). So the progressive branch is selected by the
timing table, and `ACV` then receives the full frame height instead of the per-field height.

**libogc's raw timing table** (`video.c:1514-1595`), with the mode families that reach each entry
via `__gettiming()` (`video.c:1715-1750`):

| # | Modes | EQU | ACV | PRB odd/even | PSB odd/even | nhlines | hlw | hsy | hcs/hce | hbe640 | hbs640 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | NTSC interlaced, **EURGB60 (PAL60) interlaced** | 6 | 240 | 24 / 25 | 3 / 2 | 525 | 429 | 64 | 71 / 105 | 162 | 373 |
| 1 | NTSC double-strike, EURGB60 DS | 6 | 240 | 24 / 24 | 4 / 4 | 526 | 429 | 64 | 71 / 105 | 162 | 373 |
| 2 | PAL interlaced | 5 | 288 | 33 / 34 | 1 / 0 | 625 | 432 | 64 | 75 / 106 | 172 | 380 |
| 3 | PAL double-strike | 5 | 288 | 33 / 33 | 0 / 0 | 624 | 432 | 64 | 75 / 106 | 172 | 380 |
| 4 | MPAL interlaced | 6 | 240 | 24 / 25 | 3 / 2 | 525 | 429 | 64 | 78 / 112 | 162 | 373 |
| 5 | MPAL double-strike | 6 | 240 | 24 / 24 | 4 / 4 | 526 | 429 | 64 | 78 / 112 | 162 | 373 |
| 6 | NTSC / EURGB60 / MPAL / debug **progressive** | 12 | 480 | 48 / 48 | 6 / 6 | 1050 | 429 | 64 | 71 / 105 | 162 | 373 |
| 7 | PAL progressive | 10 | 576 | 68 / 68 | 0 / 0 | 1250 | 432 | 64 | 75 / 106 | 172 | 380 |

Burst-blanking values are in §4; they are the only other per-mode data.

**The registers libogc actually programs**, for the representative mode of each family, with
`VIDEO_SetBlack(FALSE)` and the stock 640-wide centred window. `gc-video-out.md` §B.3 gives the
NTSC-interlaced row; it is repeated here only as the anchor for the others.

| Mode (`GXRModeObj`) | viYOrigin | VTR | VTO (psb_hi / prb_lo) | VTE | HTR0 | HTR1 | DI0 |
|---|---|---|---|---|---|---|---|
| `TVNtsc480IntDf` | 0 | `0F06` | `0003_0018` | `0002_0019` | `4769_01AD` | `02EA_5140` | `1107_01AE` |
| `TVNtsc240Ds` | 0 | `0F06` | `0004_0018` | `0004_0018` | `4769_01AD` | `02EA_5140` | `1108_0001` |
| `TVNtsc480Prog` | 0 | `1E0C` | `0006_0030` | `0006_0030` | `4769_01AD` | `02EA_5140` | `120E_0001` |
| `TVEurgb60Hz480IntDf` (PAL60) | 0 | `0F06` | `0003_0018` | `0002_0019` | `4769_01AD` | `02EA_5140` | `1107_01AE` |
| `TVPal528IntDf` | 24 | `1085` | `0019_0039` | `0018_003A` | `4B6A_01B0` | `02F8_5640` | `1139_01B1` |
| `TVPal576IntDfScale` | 0 | `1205` | `0001_0021` | `0000_0022` | `4B6A_01B0` | `02F8_5640` | `1139_01B1` |
| `TVPal264Ds` | 12 (doubled to 24) | `1085` | `0018_0039` | `0018_0039` | `4B6A_01B0` | `02F8_5640` | `1139_0001` |
| `TVPal528Prog` | 24 | `210A` | `0030_0074` | `0030_0074` | `4B6A_01B0` | `02F8_5640` | `1272_0001` |
| `TVMpal480IntDf` | 0 | `0F06` | `0003_0018` | `0002_0019` | `4E70_01AD` | `02EA_5140` | `1107_01AE` |

Notes on the rows that are not obvious:

* **PAL60 is NTSC.** `VIDEO_Configure()` (`video.c:2680`) writes the DCR format field only
  when the TV standard is *not* EURGB60: `if(!(HorVer.tv==VI_EURGB60)) dcr |= _SHIFTL(HorVer.tv,8,2);`
  — so EURGB60 leaves DCR bits 9:8 at 0, which is NTSC. `__gettiming()` maps EURGB60 to timing
  entry 0, which is NTSC. **Every VI register is bit-identical between `TVNtsc480IntDf` and
  `TVEurgb60Hz480IntDf`.** libogc tracks the difference only in a software variable
  (`currTvMode`), and acts on it only on the Wii, through the video DAC over I²C
  (`__VISetFilterEURGB60()`, guarded by `HW_RVL`). On a GameCube there is nothing to act on.
  An emulator cannot distinguish PAL60 from NTSC by watching the register file, and does not need
  to.
* **MPAL differs from NTSC only in colour.** Identical EQU/ACV/PRB/PSB/nhlines/hlw, so identical
  VTR/VTO/VTE/HTR1. Only HTR0's burst window (`hcs`/`hce` = 78/112 rather than 71/105) and the
  burst-blanking registers change. That is what PAL-M is: the 525/59.94 raster with a different
  subcarrier.
* **PAL `TVPal528IntDf` is a windowed mode.** `viYOrigin = (576-528)/2 = 24`, so
  `prb = div2·dispPosY = 24` and `psb = div2·((288·2) − 528 − 24) = 24` are added to the table
  values: PRB 33→57, PSB 1→25. The identity still closes: `3·5 + 57 + 2·264 + 25 = 625`.
* **`TVPal576IntDfScale` reaches 576 lines by copying, not by VI scaling.** Its `efbHeight` is 480
  and its `xfbHeight` is 576 (`video.c:1008-1021`); the 480→576 resample is the EFB copy's Y-scale
  (`gc-video-out.md` §A.3). The VI just displays a 576-line XFB. See §6.
* **`TVNtsc240Ds` has the same VTR as `TVNtsc480IntDf`** (ACV = 240 for both). The 240p-ness is
  entirely in VTO/VTE being equal, in `nhlines` being even, in DCR bit 2, and in the XFB stride —
  not in the active-line count.

---

## 4. The burst-blanking registers

BBEI (`0x14`) holds intervals **1 and 3**; BBOI (`0x18`) holds intervals **2 and 4**
(`__setBBIntervalRegs()`, `video.c:1864-1875`, and libogc's own comments name them that way). Each
interval is a start (`BSn`, 5 bits) and an end (`BEn`, 11 bits), **in half-lines within the field**
— the same unit as PRB and PSB, and the values confirm it: NTSC's `BS1 = 12`, `BE1 = 520` bracket
a 525-half-line field.

The programmed values:

| Mode | BBEI (`0x14`) | BBOI (`0x18`) | BS1/BS3 | BS2/BS4 |
|---|---|---|---|---|
| NTSC / PAL60 interlaced | `410C_410C` | `40ED_40ED` | 12 / 12 | 13 / 13 |
| NTSC / PAL60 double-strike | `410C_410C` | `410C_410C` | 12 / 12 | 12 / 12 |
| NTSC progressive | `81D8_81D8` | `81D8_81D8` | 24 / 24 | 24 / 24 |
| PAL interlaced | `4D2B_4D6D` | `4D8A_4D4C` | 13 / 11 | 12 / 10 |
| PAL double-strike | `4D6D_4D6D` | `4DAB_4DAB` | 13 / 13 | 11 / 11 |
| PAL progressive | `9B14_9B14` | `9B14_9B14` | 20 / 20 | 20 / 20 |
| MPAL interlaced | `408E_40D0` | `40ED_40AF` | 16 / 14 | 15 / 13 |

**What the four intervals are for (inference, clearly labelled).** libogc copies the table without
comment, so this is read off the *pattern*, not stated by any source:

* In **NTSC interlaced**, intervals 1 and 3 are equal and intervals 2 and 4 are equal, and the two
  groups differ by exactly one half-line — matching the odd/even PRB difference of one half-line
  (§5). So the grouping is by **field parity**: `{1,3}` for the odd field, `{2,4}` for the even.
* In **PAL and MPAL interlaced**, all four values are distinct. Both are systems with a **four-field**
  colour sequence (PAL's line-alternating phase does not repeat until the fourth field), whereas
  NTSC's repeats after two. So the four intervals are naturally read as the four fields of the
  colour sequence, with `{1,3}` being the two odd-parity fields and `{2,4}` the two even-parity
  ones — the same grouping, one level deeper.
* In every **double-strike and progressive** mode all four collapse to at most two distinct values,
  consistent with there being no field alternation to track.

**Unverified:** the polarity — whether `BS`/`BE` bracket the interval in which the colour burst is
*suppressed* or the interval in which it is *emitted*. The NTSC values (12 … 520 of 525) cover
almost the whole field, which reads more naturally as "burst emitted", but nothing read here says
so. **Unverified:** the four-field reading above; it explains the data but no source asserts it. A
capture of composite output from real hardware with the intervals perturbed would settle both; the
question is invisible to any digital-output emulator, which is why it is bottom of the list.

**For our runtime this is inert.** We do not synthesise composite video, so BBEI/BBOI are storage.
They matter only to a guest that reads them back.

---

## 5. Odd and even fields

### What tells them apart, in registers

Two things, and they agree in all eight timing entries:

1. **The parity of `nhlines`.** Interlaced entries have an *odd* half-line count per field (525,
   525, 625); double-strike and progressive entries have an *even* one (526, 624, 526, 1050, 1250).
   An odd number of half-lines per field means the next field's vertical sync arrives half a line
   out of phase with the horizontal scan — which is what interlace *is*. An even number means it
   arrives line-aligned, and the second field retraces the same scan lines: double strike.
2. **The odd/even PRB and PSB pair.** Interlaced entries have `PRB_even = PRB_odd + 1` and
   `PSB_even = PSB_odd − 1`; double-strike and progressive entries have both pairs equal.

The two are not redundant; they do different jobs. The PRB difference is what moves the even
field's picture half a scan line *down* relative to the odd field's — the active region starts one
half-line later within its own field. The compensating PSB difference keeps both fields exactly
`nhlines` long, so the field cadence stays uniform. Take either away and interlace breaks in a
different way: without the PRB offset the two fields land on top of each other; without the PSB
compensation the field period alternates.

**Attribution:** the numbers are libogc's table; the parity observation and the
"PRB up one, PSB down one" pairing are arithmetic over that table, checked in all eight entries.
The physical explanation (half-line phase ⇒ interlace) is standard raster-video behaviour, not
something these sources state.

**Which register applies to which field:** VTO (`0x0C`) to the odd (first) field, VTE (`0x10`) to
the even (second). libogc's local names in `__setVerticalRegs()` (`prbodd`/`psbodd` → `regs[6..7]`,
`prbeven`/`psbeven` → `regs[8..9]`) are unambiguous.

### The `dispPosY`-odd swap, and the "top field offset"

Two different offsets get called "the top field offset" and they are unrelated:

* **XOF** — TFBL (`0x1C`) bits 27:24, `panPosX % 16` (`video.c:1858`). This is **horizontal**, not
  vertical: the pan position is quantised to 16 pixels into the base address and the remainder is
  carried here. It exists only in TFBL; BFBL, TFBR and BFBR have no such field
  (`video.c:1928-1948`). Every stock `GXRModeObj` has `panPosX = 0`, so **XOF is 0 in every mode
  libogc ships** and nothing in this tree exercises it.
  **Unverified:** whether TFBL's XOF is applied to the bottom field's fetch as well, or only to the
  top field's. It has to apply to both for a horizontal pan to look sane, but libogc never sets it,
  so there is no evidence either way. A guest that writes a non-zero XOF and reads back the
  displayed left edge on both fields would settle it.
* **The base-address swap.** If the adjusted vertical origin is odd, `__calcFbbs()`
  (`video.c:1901-1905`) exchanges TFBL and BFBL, flipping which buffer each field reads.
  `VIDEO_GetNextField()` (`video.c:2800-2809`) undoes it in software —
  `return nextfield^(HorVer.adjustedDispPosY&0x0001);` — so a game asking libogc for the field
  parity still gets the true one. An emulator must not apply this correction itself; the swap is in
  the register values it receives, and the correction is the SDK's.

### What a game actually observes differing between the two fields

Everything else is symmetric. The observable list is short:

| Observation | How it differs |
|---|---|
| `VIDEO_GetNextField()` / `__getCurrentFieldEvenOdd()` | Derived from the position counter (§5.1): first field while the frame half-line index is below `nhlines`, second field above. |
| Which base register feeds the scan-out | TFBL for the first field, BFBL for the second, subject to the swap above. In DF mode they differ by one XFB line; in SF mode they are equal. |
| The picture's vertical position | The even field sits half a scan line below the odd, via the PRB difference. |
| Colour burst placement | BBEI vs BBOI (§4). Invisible without composite output. |
| Whether a deferred mode change lands | `__VISetRegs()` (`video.c:2360-2366`) refuses to apply a queued *mode* change unless the current field is the first one. A game that changes video mode and then busy-waits is waiting on field parity. |
| The display interrupts | **Nothing.** DI0–DI3 match on a frame-absolute position (§7); they do not know about fields. DI0 happens to land on the second field's first half-line, but that is where libogc put it, not a property of the register. |

`retraceCount`, and therefore `VIDEO_WaitVSync()`, advances **once per field** — not once per
frame — because DI0 and DI1 between them produce one interrupt per field and the handler
increments once per invocation regardless of which fired.

---

## 5.1 The display-position register

`gc-video-out.md` §B.1 records DPV at `0x2C` and DPH at `0x2E`, both 11-bit and 1-based. This
section answers the three questions it leaves open: what the units are, what the counters' ranges
are, and whether our implementation is right.

### What DPH counts

**DPH counts VI pixels within the line, 1-based, from 1 to `2 × hlw`** — 1…858 for NTSC, 1…864 for
PAL. It is the same unit as every other horizontal register (§1), i.e. **two** of `gc.h`'s 27 MHz
VI clocks per count.

**Evidence (libogc, and it is decisive).** `__getCurrentHalfLine()` (`video.c:2336-2348`):

```c
hpos--;
vpos--;
vpos <<= 1;
return vpos+(hpos/currTiming->hlw);
```

The expression adds `hpos/hlw` to a doubled line number, so it **must** yield 0 or 1 — the index of
the half-line within the line. That constrains `hpos − 1 < 2·hlw`, i.e. DPH ≤ 858. If DPH counted
27 MHz clocks (1…1716) the quotient would run 0…3 and the half-line index would be wrong for three
quarters of every line.

### What DPV counts

**DPV counts whole lines, 1-based, across a period of two `nhlines` intervals — not one field.**
For NTSC interlaced that is 1…525, a full frame. For PAL interlaced, 1…625.

**Evidence (libogc).** Three independent places require the counter to span more than one field:

1. `__getCurrentFieldEvenOdd()` (`video.c:2350-2358`) returns "first field" when the half-line
   index is below `nhlines` and "second field" otherwise. If DPV reset every field the second
   branch would be unreachable.
2. `VIDEO_GetCurrentLine()` (`video.c:2916-2928`) explicitly folds the second field back:
   `if(curr_hl>=currTiming->nhlines) curr_hl -= currTiming->nhlines;` — a no-op unless the counter
   exceeds `nhlines`.
3. `__VIDisplayPositionToXY()` (`video.c:2383-2451`) branches on the same comparison to choose
   between the odd field's PRB/PSB and the even field's.

**Evidence (arithmetic).** `nhlines` half-lines is always *exactly one retrace period*, in every
mode:

| Mode | nhlines (half-lines) | × half-line duration | = period | rate |
|---|---|---|---|---|
| NTSC interlaced | 525 | × 31.78 µs | 16.68 ms | 59.94 Hz |
| NTSC double-strike | 526 | × 31.78 µs | 16.72 ms | 59.83 Hz |
| NTSC progressive | 1050 | × 15.89 µs | 16.68 ms | 59.94 Hz |
| PAL interlaced | 625 | × 32.0 µs | 20.0 ms | 50 Hz |
| PAL progressive | 1250 | × 16.0 µs | 20.0 ms | 50 Hz |

and DPV's maximum is `nhlines` *lines* = `2 × nhlines` half-lines = two such periods. That is what
makes libogc's interrupt placement (§7) come out right in every mode with one formula.

### Is it one-based?

Yes, both of them. `__getCurrentHalfLine()` decrements both before use, and libogc programs DI0's
HCT as `hlw + 1` and DI1's as `1` — a 0-based counter would make DI1's match position `hlw`
clocks into the line instead of at its start.

### Is our implementation right?

```c
/* gc_io.c:55-58 */
static uint32_t vi_position_read(GcMachine *m, uint32_t stored) {
  return ((m->vi_line + 1u) << 16) | (m->vi_column + 1u);
}
```

with, from `gc.c:69-80`:

```c
const uint32_t vi_clock = (uint32_t)(into_field / GC_CYCLES_PER_VI_CLOCK);
m->vi_line   = vi_clock / GC_VI_CLOCKS_PER_LINE;   /* 1716 */
m->vi_column = vi_clock % GC_VI_CLOCKS_PER_LINE;
... at the field boundary: m->vi_line = 0; m->vi_column = 0;
```

**The `+1` on both halves is right, and the packing is right.** DPV in the high half-word at
`0x2C` and DPH in the low half-word at `0x2E` matches libogc's `_viReg[22]`/`_viReg[23]`, and our
register file serves a 16-bit read of either half out of the one 4-byte entry correctly
(`io_read_one`'s partial-read path, `gc_io.c:970-976`).

**Two things are wrong, and both follow from having tuned the value against
`VIDEO_GetCurrentLine()` alone.** That function is unusually forgiving: it takes the half-line
index modulo `nhlines` and halves it, so almost any monotonically advancing pair produces a
plausible-looking line number. It cannot detect either fault.

1. **`vi_column` is in 27 MHz VI clocks (0…1715); DPH must be in VI pixels (1…858).** Ours is
   twice the hardware range in twice-too-fine a unit. Effect: `hpos/hlw` yields 0…3 instead of 0…1,
   so `__getCurrentHalfLine()` overstates the half-line index by up to 2 and
   `VIDEO_GetCurrentLine()` is off by up to one line and steps unevenly within a line.
   **Fix:** divide by `2 * GC_CYCLES_PER_VI_CLOCK` rather than `GC_CYCLES_PER_VI_CLOCK`, i.e.
   `vi_column` should run 0…857 and `vi_line` 0…262.
2. **`vi_line` resets every field; DPV must run across two fields.** Ours yields DPV 1…263 and
   never reaches 264…525. Effect: the half-line index never exceeds ~527, so
   `__getCurrentFieldEvenOdd()` returns "first field" for about 99.9% of every field, and
   `VIDEO_GetNextField()` returns a near-constant. A game that renders alternate fields differently
   never alternates. (The one thing this does *not* break is the deferred mode change:
   `__VISetRegs()` needs "first field" to proceed, and ours nearly always says so, so mode changes
   apply rather than hanging. That is luck, not design.)
   **Fix:** count the line within a *two-field* period — `vi_line` 0…524 for NTSC interlaced,
   resetting every second field boundary rather than every one.

Both faults are invisible to `tests/gc/programs/gc_timetest.c` as it stands, which checks only that
`VIDEO_GetCurrentLine()` advances (`gc_timetest.c:201-213`) and that 300 `VIDEO_WaitVSync()` calls
take the right wall time. §9 lists tests that would catch them.

---

## 6. The vertical scaler — and why there isn't one

### What the interface can scale

Three registers, all horizontal:

| Register | Field | Meaning |
|---|---|---|
| HSW (`0x48`) | STD, bits 7:0 | **Stride**: 32-byte words to advance between one displayed line and the next. |
| HSW (`0x48`) | WPL, bits 14:8 | **Line width**: 32-byte words (16 pixels each) fetched per displayed line. |
| HSR (`0x4A`) | STP, bits 8:0 | **Horizontal step**, 1.8 fixed point: source pixels advanced per output pixel. `0x100` = 1:1. |
| HSR (`0x4A`) | bit 12 | Horizontal scaling enable. |
| FBWIDTH (`0x70`) | 15:0 | Source pixels per line fed to the horizontal scaler. |

`__setPicConfig()` (`video.c:1852-1862`) and `__setScalingRegs()` (`video.c:1877-1889`) are the
only writers:

```c
*wordPerLine = (fbSizeX+15)/16;
*std = *wordPerLine;
if(xfbMode==VI_XFBMODE_DF) *std <<= 1;
*xof = panPosX%16;
*wpl = (*xof+(panSizeX+15))/16;
regs[36] = (*wpl<<8)|*std;
...
if(panSizeX<dispSizeX) {
    regs[37] = 0x1000|((dispSizeX+(_SHIFTL(panSizeX,8,16)-1))/dispSizeX);
    regs[56] = panSizeX;
} else regs[37] = 0x100;
```

`regs[37]`'s value is `ceil(256 · panSizeX / dispSizeX)`, and the guard `panSizeX < dispSizeX`
means the enable bit is set **only for upscaling**. Every stock `GXRModeObj` has
`panSizeX == fbWidth == 640` and `dispSizeX == viWidth == 640`, so **the horizontal scaler is
disabled in every mode libogc ships** — `regs[37] = 0x100`, and FBWIDTH keeps the fixed 640 that
`VIDEO_Init()` writes (`video.c:2570`). It exists for a game that renders a narrow framebuffer and
stretches it.

### There is no vertical counterpart

**There is no vertical step register, no vertical scale enable, and no vertical source-height
register.** Claims that rest on this, and their evidence:

* libogc programs none. Every VI register it touches is accounted for in `gc-video-out.md` §B.1,
  and the only vertical ones are VTR/VTO/VTE (how many lines to display and where) and HSW's STD
  (how far apart they are in memory).
* The one mode in the whole set that resamples vertically —`TVPal576IntDfScale`, which puts a
  480-line render on a 576-line PAL picture — does it entirely in the **copy**: `efbHeight = 480`,
  `xfbHeight = 576` (`video.c:1008-1021`), and the application is expected to pass
  `xfbHeight/efbHeight` to `GX_SetDispCopyYScale()` (`gc-video-out.md` §A.3). If the VI could scale
  vertically, that mode is exactly where libogc would have used it, and it does not.

**So: all vertical resampling in the GameCube display path happens at EFB-copy time.** The
interface reads the XFB line for line. An emulator must implement vertical scaling once, in the
copy, and never in the scan-out.

### How a 480-line buffer becomes two 240-line fields

Not by scaling. By **stride and base offset**:

* STD is doubled for `VI_XFBMODE_DF` (`video.c:1856`), so each field advances *two* XFB lines per
  displayed line.
* BFBL is set one XFB line below TFBL (`video.c:1899`), so the second field starts on the other
  parity.
* WPL is **not** doubled — the fetch is still 640 pixels (40 words) per displayed line.
* ACV is 240, so each field displays 240 lines.

Field one therefore reads XFB lines 0, 2, 4 … 478 and field two reads 1, 3, 5 … 479. Two 240-line
fields, 480 distinct lines, no resampling anywhere.

The mirror case — a **240-line buffer on a 480-line interlaced picture** — is `VI_XFBMODE_SF`:
STD not doubled, `BFBL == TFBL`, both fields fetch the same 240 lines. What the two fields show
then depends on the game, not the VI:

* `TVNtsc240Ds` (`video.c:90-122`): double-strike timing, so the two fields coincide on screen —
  true 240p, and `field_rendering` is `GX_FALSE`.
* `TVNtsc240Int` (`video.c:158-190`): *interlaced* timing with an SF buffer, so the two fields are
  half a line apart but read the same memory. `field_rendering` is `GX_TRUE` precisely because the
  game is expected to re-render and re-copy between the two fields, with the camera shifted half a
  line, so that the shared buffer holds different content each time. This is the one configuration
  where the XFB is rewritten twice per frame.

### The anti-flicker filter in the interface

There isn't one either, in the sense the name suggests. Deflicker — the 7-tap vertical blend that
stops a 480i picture shimmering — is the **copy** filter, BP `0x53`/`0x54`, applied once at copy
time (`gc-video-out.md` §B.6 has the tap sets; they are not repeated here). What reaches the
interface is already deflickered, and the interface has no vertical taps to apply.

The VI *does* have a coefficient table — FCT0–FCT6 at `0x4C`–`0x67`, 26 entries — but it is
horizontal-shaped and mode-independent: `VIDEO_Init()` writes one fixed constant
(`video.c:2556-2569`) and nothing ever changes it, not even between NTSC and PAL, interlaced and
progressive, AA and non-AA. A vertical anti-flicker filter would have to vary with at least the
interlace mode; this does not. **Unverified:** what the table actually filters (already recorded in
`gc-video-out.md` §B.6). The useful addition here is the negative claim, which *is* supported: it
is not the deflicker filter, because the deflicker filter is demonstrably elsewhere and varies per
mode while this does not. Writing the constant and otherwise ignoring it reproduces libogc exactly.

---

## 7. The four display interrupts

### What arms a match

Each of DI0–DI3 (`0x30`, `0x34`, `0x38`, `0x3C`) is one 32-bit register:

| Bits | Field |
|---|---|
| 9:0 | **HCT** — horizontal count to match, 1-based, in VI pixels (§1) |
| 25:16 | **VCT** — vertical count to match, 1-based, in lines (§5.1) |
| 28 | enable |
| 31 | status — set by the match, **write 0 to acknowledge** |

The interrupt fires when the display position reaches (VCT, HCT). In libogc's half-line
vocabulary the target is `halfline = (VCT − 1)·2 + (HCT − 1)/hlw`.

All four OR into the single Processor Interface video IRQ (`IRQ_PI_VI`, `video.c:2617`; our
`GC_INT_VIDEO = 8`, `gc.h:55`). `__VIRetraceHandler()` (`video.c:2462-2493`) polls all four status
bits, acknowledges each that is set, and increments `retraceCount` **once per invocation**
regardless of how many fired. The status bit must be sticky until written clear or the handler
spins.

### How libogc places them, and why the formula works in every mode

`__setInterruptRegs()` (`video.c:1840-1850`) is the whole of it:

```c
hlw = 0;
if(tm->nhlines%2) hlw = tm->hlw;
regs[24] = 0x1000|((tm->nhlines/2)+1);   /* DI0: enable + VCT */
regs[25] = hlw+1;                        /* DI0: HCT */
```

and DI1 is a constant `_viReg[26] = 0x1001; _viReg[27] = 0x0001;` — enable, VCT = 1, HCT = 1
(`video.c:2131-2132`), never recomputed.

Read with §5.1's result that DPV spans two `nhlines` periods, this says exactly one thing:

> **DI1 marks the top of the first period; DI0 marks the top of the second.**

DI0's half-line target is `((nhlines/2 + 1) − 1)·2 + (hlw+1−1)/hlw`, which is `nhlines` when
`nhlines` is odd (the `+hlw` picks the second half-line of the line the boundary falls inside) and
`nhlines` when it is even (the boundary falls at a line start, so HCT = 1 suffices). The
`nhlines % 2` test is not a special case — it is the same target expressed for the two possible
alignments. Because `nhlines` half-lines is one retrace period in every mode (§5.1), the pair
always produces **one interrupt per retrace period**: 59.94 Hz for NTSC, 50 Hz for PAL.

### The order across a field, for NTSC interlaced

| Frame half-line | Interrupt | Position |
|---|---|---|
| 0 | **DI1** | first half-line of the first (odd) field |
| 525 | **DI0** | first half-line of the second (even) field |
| — | DI2, DI3 | wherever the game put them; libogc programs neither |

DI2 and DI3 are the raster-effect interrupts. `__VIRetraceHandler()` computes a raster position and
calls `positionCB` only when one of them fired (`video.c:2496-2500`) — and `positionCB` is a
file-static with no setter in libogc (`video.c:1624`), so the path is dead in the stock SDK and
exists for a game that drives the VI itself.

### The one place this is Unverified

In **interlaced** modes the two-field span of DPV is forced: VTO and VTE differ, so the counter
demonstrably distinguishes them. In **double-strike and progressive** modes the two fields are
identical, and libogc's DI0 target then falls one line past the end of a single frame
(VCT = 264 for NTSC DS against 263 lines; VCT = 526 for NTSC progressive against 525).

**Unverified:** whether DI0 fires at all in those modes. Two readings fit libogc equally well and
give the same retrace rate:

* DPV wraps after one frame, DI0 never matches, and DI1 alone supplies the retrace — one per frame,
  which is 59.94 Hz in progressive and 59.83 Hz in 240p, both correct.
* DPV wraps after two frames as in interlaced, DI0 fires at the second frame's top, and the pair
  supplies one retrace per frame — also correct.

`gc-video-out.md` §B.4 asserts the second reading for progressive ("retraceCount still advances
twice per frame"). It may well be right, but it is not established by anything read here.
**Evidence that would settle it:** on hardware, or on a runtime under test, configure
`TVNtsc480Prog`, enable DI0 *only*, and count interrupts over a second — 0 or 59.94. `gc_timetest.c`
already has the harness for exactly this shape of measurement.

### What our runtime does instead

```c
/* gc_io.c:267-275 */
void gc_vi_field_boundary(GcMachine *m) {
  for (i = 0; i < 4; i++) {
    uint32_t *slot = io_slot(m, "vi", offsets[i]);
    if (slot && (*slot & 0x10000000u)) *slot |= 0x80000000u;
  }
  gc_vi_update_interrupt(m);
}
```

Every *armed* interrupt is raised at every field boundary, and **VCT and HCT are ignored entirely**.
The acknowledge protocol is modelled correctly — the status bit is storage, a 16-bit write of the
high half-word merges properly (`gc_io.c:1011-1016`), and the IRQ line drops when no
enabled-and-pending register is left (`gc_io.c:254-265`).

The consequences are bounded but real:

* **Retrace rate: correct.** libogc arms DI0 and DI1, both fire together once per field, the handler
  increments once, so `retraceCount` runs at 59.94 Hz — the right answer by a wrong route.
* **DI2/DI3 raster splits: wrong.** A game that arms DI2 at mid-screen gets it at the top of the
  field instead, so any mid-frame register change lands 240 lines early.
* **A game that arms only DI0: wrong rate.** Ours fires it every field; hardware fires it once per
  two `nhlines` periods, i.e. every other field in interlaced modes.
* **A game that enables an interrupt and never programs VCT/HCT** (leaving them zero) gets an
  interrupt from us and, on hardware, a match at line 0 / column 0 which is never reached by a
  1-based counter — i.e. nothing.

The fix is the same shape as the position fix: once `vi_line`/`vi_column` are in hardware units and
span two fields (§5.1), raising each DI when the beam crosses its (VCT, HCT) is a direct
comparison, and DI0/DI1 land where libogc expects without any field-boundary special case.

---

## 8. The two base registers, and what one buffer gets you

The interface holds four base addresses: TFBL (`0x1C`) and BFBL (`0x24`) for the left eye, TFBR
(`0x20`) and BFBR (`0x28`) for the right. `gc-video-out.md` §B.2 covers the address encoding and
§B.5 the DF/SF relationship; what follows is what they mean for a game.

**The interface always reads two buffers.** There is no "use one buffer" bit. In SF mode the two
registers simply hold the same address, which libogc arranges explicitly
(`*bfbb = *tfbb;` before the DF branch, `video.c:1897-1899`). An emulator must read BFBL for the
second field unconditionally.

**libogc never lets a game set only one.** Both `VIDEO_SetFramebuffer()` (`video.c:2715-2737`) and
`VIDEO_SetNextFramebuffer()` (`video.c:2739-2751`) take a single pointer and call `__setFbbRegs()`,
which derives both. The difference between them is *when*: `VIDEO_SetFramebuffer()` writes
`_viReg[14,15,18,19]` immediately, bypassing the deferred-flush machinery; `VIDEO_SetNextFramebuffer()`
only updates the shadow, and the write lands in the retrace handler. A game flipping buffers every
frame uses the latter, so the flip is atomic with respect to scan-out.

**A game that writes only TFBL** — which means a game driving the registers directly, not through
libogc — gets its picture on the first field and whatever BFBL last held on the second. Since
`__VIInit()` never writes the base registers at all, that is address 0 out of reset: the second
field scans out the bottom of the exception-vector area as YUV. The visible result is a picture
that alternates with noise at the field rate, which looks like severe flicker rather than like a
missing frame. **This is inference from the register structure, not observed** — nothing in this
tree runs such a program. `gc_drawtest.c` could be made to do it in about ten lines (§9).

**The right-eye registers** are written only when `HorVer.threeD` is set (`video.c:1937-1948`),
which happens only for the mode combination `VI_NON_INTERLACE|VI_PROGRESSIVE`
(`video.c:2661`), flagged in DCR bit 3. No shipped `GXRModeObj` uses it. For our runtime they
are storage, correctly.

---

## 9. Tests a guest program could run against our runtime

These are ordered by what they would catch, and each is a small addition to an existing test
program rather than a new one. `gc_timetest.c` already links libogc, configures a mode, and has
`check_within()` and `GC_CHECK()`; `gc_drawtest.c` already renders and copies.

**T1 — DPH's range and unit.** *(catches §5.1 fault 1)*
Poll `_viReg[23]` (`0xCC00202E`) in a tight loop for one field and record the maximum. Expect
**858** for NTSC, 864 for PAL. Ours returns up to 1716. A one-line assertion.

**T2 — DPV's range and span.** *(catches §5.1 fault 2)*
Poll `_viReg[22]` across several fields and record the maximum. Expect **525** for
`TVNtsc480IntDf`. Ours returns at most 263. Pair it with: count how many times the maximum is
reached over 120 fields — hardware reaches it 60 times (once per frame), we would reach 263 every
field.

**T3 — field parity alternates.** *(catches §5.1 fault 2 from the API side)*
Call `VIDEO_GetNextField()` once per `VIDEO_WaitVSync()` for 60 fields and count transitions.
Expect **59** — it alternates every field. Ours would return a near-constant. This is the cheapest
test of the three and the one a real game would notice.

**T4 — the vertical accounting identity.** *(pins §3)*
After `VIDEO_Configure()` + `VIDEO_Flush()` + a retrace, read VTR, VTO and VTE back and assert
`3·EQU + PRB + 2·ACV + PSB == nhlines` for both fields, with `nhlines` hard-coded per mode from the
table in §3. Run it for `TVNtsc480IntDf`, `TVNtsc240Ds` and `TVNtsc480Prog`. This is a pure
register-readback test; it validates that our register file is not mangling the fields on the
partial-width writes libogc performs (it writes VTO as two separate 16-bit stores).

**T5 — the two fields read different addresses.** *(pins §6 and §8)*
Allocate a 480-line XFB, fill even lines with one colour and odd lines with another, and read
TFBL and BFBL back. Assert `BFBL − TFBL == 1280` for a DF 640-wide mode and `BFBL == TFBL` for an
SF one. Then, for an emulator-side check, dump the scanned-out frame and assert that both colours
are present — a runtime that reads only TFBL would show one.

**T6 — DI2 fires where it is aimed.** *(catches §7)*
Install a handler on `IRQ_PI_VI`, program DI2 with VCT = 132 (mid-picture for NTSC 480i) and
HCT = 1, enable it, and record `VIDEO_GetCurrentLine()` inside the handler. Expect ~131. Ours
would report ~0, because the interrupt arrives at the field boundary. This is the single most
valuable test in the list: it is the one thing in this document that would visibly break a game.

**T7 — DI0 alone, in interlaced and in progressive.** *(settles the Unverified in §7)*
Disable DI1, leave DI0 armed, count interrupts over one second. In `TVNtsc480IntDf` expect
**29.97** (once per two fields) — ours would give 59.94. In `TVNtsc480Prog` the answer is either 0
or 59.94 and the measurement is the evidence that decides which; record it rather than asserting
it. Needs care: libogc's own retrace handler must not be the one acknowledging, or
`VIDEO_WaitVSync()` will deadlock — run it with libogc's video IRQ handler replaced.

**T8 — XOF applies to both fields.** *(settles the Unverified in §5)*
Write a non-zero XOF into TFBL bits 27:24 with a DF buffer whose two field-parities hold different
patterns, and compare the horizontal offset of the two fields in the output. Equal offsets mean XOF
is global; unequal means it is per-register. Low value for us — nothing sets XOF — but it is the
only way to answer the question.

**Not testable from a guest:** the burst-blanking polarity and the four-field reading (§4), and the
function of the FCT table (§6). All three are properties of composite encoding, which our runtime
does not produce and which no digital-output emulator can observe.

---

## Summary of what is not established

In descending order of how much it would cost to be wrong:

1. **Whether DI0 fires in double-strike and progressive modes** (§7). Both readings give the right
   retrace rate, so nothing visible depends on it today; it matters the moment a game arms DI0
   alone. Test T7.
2. **Whether TFBL's XOF applies to the bottom field** (§5). Zero in every stock mode. Test T8.
3. **Whether HBE is counted from the first half-line specifically** (§2), as opposed to some other
   encoding that yields the same active width. The width identity holds regardless.
4. **The burst-blanking polarity and the four-field interpretation** (§4). Invisible without
   composite output.
5. **What the VI's 26-entry coefficient table filters** (§6, already recorded in
   `gc-video-out.md` §B.6). Reproducing the constant matches libogc exactly.

And one thing that *is* established and is worth repeating because it inverts the natural
assumption: **the video interface does no vertical scaling and no vertical filtering.** Both belong
to the EFB copy. If an emulator finds itself wanting a vertical resample during scan-out, it has
put something in the wrong stage.
