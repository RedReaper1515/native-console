# The GameCube Memory Card: the EXI protocol and the on-card layout

This document covers the memory card as two separate problems that meet at one place. The first is
the **bus protocol** — which EXI channel and device a card answers on, how software finds out it is
there, and the exact byte sequences of the commands a game issues during a save. The second is the
**on-card layout** — the five system blocks at the front of the card, their checksums, and the
directory entry that describes a save file. They meet at formatting: §F says what an image we
produce ourselves has to contain before the SDK's CARD library will mount it instead of asking the
user to format.

It is the companion to [`gc-video-out.md`](gc-video-out.md) and follows the same conventions. The
EXI register *block* is described here only as far as the memory card needs it; nothing else in the
repository documents it, so §A.2 is the only account of the transfer registers.

## Provenance

Four sources, all read directly:

1. **devkitPro/libogc `card.c`** (zlib licence), fetched 2026-09-12 from
   `https://raw.githubusercontent.com/devkitPro/libogc/master/libogc/card.c` (3296 lines; all line
   numbers below refer to that revision). This is the primary authority for everything in Parts B
   through F: it issues the commands byte by byte and it declares the on-card structures as C
   structs, with no abstraction in between.
2. **devkitPro/libogc `exi.c`** (zlib licence), same fetch, 970 lines. The authority for Part A —
   the register block, the shape of an immediate and a DMA transfer, and the detection path.
3. **devkitPro/libogc `system.c`** (zlib licence), same fetch, 1754 lines. Used only for the SRAM
   device (§F.2, §F.3), because the card's mount path reads two fields out of the console's
   settings block.
4. **libogc headers** `D:\devkitPro\libogc\include\ogc\{card.h, exi.h, system.h}` for the API
   enumerations, the error codes, the directory-entry struct, and the SRAM structs. `card.h` is the
   best evidence for what the API promises callers, and it is the one place the *sizes* of the
   directory-entry fields are stated as a struct a compiler lays out.

One **cross-check against a second implementation**, used as a behaviour reference only and
described in our own words, never copied: **Dolphin**'s EXI memory-card device and its SRAM
handling (GPL — read, not reused). Where it is cited below it is labelled as such, and it is never
the sole support for a claim that an emulated card must satisfy.

**No hardware test exists for any of this.** `roms/gc/hwtests/` contains `cputest`, `gxtest` and
`iostest`; none of them touches EXI or the memory card. So every claim here rests on libogc's
source, on the arithmetic that has to be true for libogc's own structures to fit, or on the
observed behaviour of `runtimes/runtime-gc`. Each claim below says which. Anything not established by those
is marked **Unverified:**.

A note on what libogc is and is not evidence *for*. libogc is an independent reimplementation of
the SDK's CARD library, not the SDK. Where libogc *issues* a command, that is strong evidence about the
hardware — the bytes went to a real card and real cards answered. Where libogc *checks* something,
that is evidence only about libogc: the retail CARD library may check more (§F.4 lists the three
places this matters). Commercial games link the retail library, so the distinction decides whether
a card image we write will actually mount.

---

# Part A — the bus

## A.1 Which channel, which device

| Slot | EXI channel | EXI device |
|---|---|---|
| Slot A | 0 | 0 |
| Slot B | 1 | 0 |

**Evidence.** `card.h:47-48` defines `CARD_SLOTA` as 0 and `CARD_SLOTB` as 1; `exi.h:62-63`
annotates `EXI_CHANNEL_0` as "memory card slot A" and `EXI_CHANNEL_1` as "memory card slot B". The
slot number *is* the channel number — `card.c` passes its `chn` argument straight into every
`EXI_*` call. Every card operation selects **device 0** explicitly: `EXI_Select(chn, EXI_DEVICE_0,
EXI_SPEED16MHZ)` appears at `card.c:839`, `866`, `890`, `914`, `939`, `1046`, `1444` and `2151`, with no other
device number anywhere in `card.c`.

Two consequences an emulator must get right:

* **Channel 2 is not a card slot.** Every entry point in `card.c` begins `if(chn<EXI_CHANNEL_0 ||
  chn>=EXI_CHANNEL_2) return CARD_ERROR_NOCARD;` — channel 2 is excluded by the bound. `exi.h:64`
  calls it "other EXI devices", and `exi.c:134` notes it is used only when a debugger is attached.
* **Device 0 on channel 0 is the card; device 1 on channel 0 is something else entirely** — the
  RTC, the 64-byte settings SRAM and the font ROM share it (`system.c:511`, `538`, `556`). That is
  the device `runtimes/runtime-gc` already answers (§A.4). A card and the settings block live on the same
  channel and are distinguished only by the device-select bits in the channel's control/status
  register.

The card is selected at **16 MHz** (`EXI_SPEED16MHZ`, `exi.h:96`) for every operation except the ID
read, which `EXI_GetID()` performs at 1 MHz (`exi.c:611`). The frequency only sets bits 6:4 of the
status register (§A.2) and nothing downstream of that reads them back, so an emulated card can
ignore it — but a card that *checks* the frequency would have to accept both.

## A.2 The EXI register block and the shape of a transfer

The block starts at **`0xCC006800`** (`exi.c:101`) and libogc addresses it as a `volatile u32`
array with a **stride of five words (20 bytes) per channel**: `_exiReg[nChn*5 + k]`.

| Word | Byte offset (channel *n*) | Name | Role |
|---|---|---|---|
| `nChn*5 + 0` | `0x00 + 0x14n` | **CSR** — status/control | device select, frequency, interrupt status and masks, the external-device bit |
| `+ 1` | `0x04 + 0x14n` | **DMA address** | written as `(u32)pData & 0x03FFFFE0` (`exi.c:552`) — a 26-bit physical address, 32-byte aligned |
| `+ 2` | `0x08 + 0x14n` | **DMA length** | in bytes (`exi.c:553`) |
| `+ 3` | `0x0C + 0x14n` | **CR** — transfer control | the transfer request; bit 0 is TSTART |
| `+ 4` | `0x10 + 0x14n` | **immediate data** | one 32-bit word of payload |

This is exactly the layout `runtimes/runtime-gc/src/gc_io.c:1048-1058` already declares (`exi_regs`), with
the block based at `0x6800` and 0x40 bytes long (`gc_io.c:1075`) — three channels of 0x14 is 0x3C.

### The CSR bits libogc uses

| Bit | Value | Meaning | Evidence |
|---|---|---|---|
| 0 | `0x0001` | EXI-interrupt mask | preserved across select/deselect by the `&0x405` mask, `exi.c:336`, `exi.c:417` |
| 1 | `0x0002` | **EXI interrupt status** (the *device* is asking for attention) | `EXI_EXI_IRQ`, `exi.c:56`; written back set to acknowledge, `exi.c:114-118` |
| 2 | `0x0004` | transfer-complete mask | preserved by `&0x405` |
| 3 | `0x0008` | transfer-complete interrupt status | `EXI_TC_IRQ`, `exi.c:57` |
| 6:4 | | frequency | `(nFrq<<4)`, `exi.c:336` |
| 7 | `0x0080` | select device 0 | `EXI_DEVICE0`, `exi.c:52`; set as `0x80<<nDev`, `exi.c:336` |
| 8 | `0x0100` | select device 1 | `EXI_DEVICE1`, `exi.c:53` |
| 9 | `0x0200` | select device 2 | `EXI_DEVICE2`, `exi.c:54` |
| 10 | `0x0400` | external-insertion interrupt mask | preserved by `&0x405` |
| 11 | `0x0800` | **external-insertion interrupt status** — the device was removed or inserted | `EXI_EXT_IRQ`, `exi.c:58` |
| 12 | `0x1000` | **EXT — a device is physically present in this slot** | `EXI_EXT_BIT`, `exi.c:59`, read at `exi.c:182` |
| 13 | `0x2000` | **Unverified:** `__exi_init()` writes `_exiReg[0] = 0x2000` into channel 0's CSR once, with no comment (`exi.c:743`), and nothing reads it back. It is conventionally called the boot-ROM disable, but no source read here says so. | |

The three mask bits (0, 2, 10) are the only CSR bits libogc *preserves* when it selects or
deselects: `val = (val&0x405)|(0x80<<nDev)|(nFrq<<4)` on select (`exi.c:336`) and
`_exiReg[nChn*5] = (val&0x405)` on deselect (`exi.c:417`). **Chip-select is therefore visible to a
device only as a write to the CSR** — there is no separate select strobe. §A.4 explains why that
matters for the runtime as it stands.

### An immediate transfer, and the byte order

This is the detail most likely to be got backwards, so it is stated twice, in both directions.

**Write** (`exi.c:489-494`):

```c
for(i=0,value=0;i<nLen;i++) value |= (((u8*)pData)[i])<<((3-i)*8);
_exiReg[nChn*5+4] = value;
...
_exiReg[nChn*5+3] = (((nLen-1)&0x03)<<4)|((nMode&0x03)<<2)|0x01;
```

**Read** (`exi.c:454`, inside `EXI_Sync()`):

```c
val = _exiReg[nChn*5+4];
for(i=0;i<cnt;i++) ((u8*)buf)[i] = (val>>((3-i)*8))&0xFF;
```

So, for a transfer of *n* ≤ 4 bytes:

| Wire byte | Immediate-register bits |
|---|---|
| first (index 0) | **31:24** |
| second | 23:16 |
| third | 15:8 |
| fourth | 7:0 |

**The first byte on the wire is the most significant byte of the immediate register, in both
directions.** A one-byte read — which is how the status byte comes back (§B.2) — must therefore
present its value in bits **31:24**, not in bits 7:0. A device that puts a status byte in the low
byte will be read as zero, and `status & CARD_STATUS_UNLOCKED` will be false, which sends libogc
down the DSP unlock path (§F.2) rather than to a clean error.

Unused bytes of a short transfer are simply not looked at; `EXI_Imm()` masks the length into CR
bits 5:4 as `(nLen-1)&3`, so a 1-byte transfer is TLEN 0 and a 4-byte transfer is TLEN 3.

### CR — the transfer control word

| Bits | Field | Values |
|---|---|---|
| 0 | **TSTART** | write 1 to begin; hardware clears it when the transfer finishes |
| 1 | **DMA** | 0 = immediate, 1 = DMA |
| 3:2 | **RW** | 0 = read, 1 = write, 2 = read-write (`exi.h:48-50`) |
| 5:4 | **TLEN** | immediate transfers only: bytes − 1 |

Immediate: `(((nLen-1)&3)<<4) | ((nMode&3)<<2) | 0x01` (`exi.c:494`).
DMA: `((nMode&3)<<2) | 0x03` (`exi.c:554`) — TSTART and DMA together, length and address in their
own registers.

`EXI_Sync()` opens with `while(_exiReg[nChn*5+3]&0x0001);` (`exi.c:443`) — **a spin on TSTART**.
This is the loop that a machine which does not model EXI never leaves, and it is why `gc_io.c`
clears TSTART inside the write that set it (`gc_io.c:561`).

### DMA

The address register takes `(u32)pData & 0x03FFFFE0` — the low 26 bits of the *physical* address,
with the bottom five bits forced to zero. So **DMA is 32-byte aligned and reaches only the 64 MB
that 26 bits cover**, which for the GameCube is all of MEM1 and more. The length is a plain byte
count, and every card DMA is a multiple of 32 (128 for a page write, 512 for a segment read).

## A.3 Detection: how software decides a card is there

This is the path that matters most, because its two failure modes are very different: a slot that
reports nothing gets a polite "no memory card" from the game, and a slot that reports a card but
then does not answer gets a hang.

`CARD_Probe(chn)` is `EXI_Probe(chn)` and nothing else (`card.c:2448-2451`). The chain is:

**1. `__exi_probe()`** (`exi.c:164-206`), for a channel with no device attached yet:

* If CSR bit 11 (`EXI_EXT_IRQ`) is set, acknowledge it and forget any cached ID.
* If CSR bit 12 (`EXI_EXT_BIT`) is **clear**, return 0 immediately — *no card*.
* If bit 12 is set, apply a debounce: remember the time it was first seen, and return 0 until
  roughly 30 µs have passed (`if((val=diff_usec(last_exi_idtime[nChn],time)+10)<30) ret = 0;`,
  `exi.c:185`). Only after that does it return 1.

  **This means the first probe after insertion — or after boot — legitimately returns 0.** Software
  is expected to call again. If the machine's time source does not advance, `diff_usec` never grows
  and the probe returns 0 for ever; a game that treats that as "no card" carries on, and a game
  that polls until it changes hangs. The dependency is on `SYS_Time()`, i.e. on the Gekko timebase,
  which `runtimes/runtime-gc` already advances for the VI.

* For a channel that *is* attached, the test inverts: if bit 12 has gone clear or bit 11 has gone
  set, the card is considered gone and the cached ID is dropped.

**2. `EXI_Probe()`** (`exi.c:696-710`) then reads the ID if one is not cached, via `EXI_GetID()`.

**3. `EXI_GetID()`** (`exi.c:576-638`) is the actual bus transaction. For channel < 2 and device 0
it first probes and attaches (unmasking the insertion interrupt), then:

* lock the channel, select device 0 at 1 MHz;
* **immediate write of 2 bytes, both zero** (`reg = 0; EXI_Imm(nChn,&reg,2,EXI_WRITE,NULL)`);
* **immediate read of 4 bytes** into the caller's `u32`;
* deselect, unlock, detach; cache the value against a timestamp.

So **the ID command is two zero bytes followed by a four-byte read**, and the result is the 32-bit
immediate register read back with the wire-order rule of §A.2 — which, on a big-endian machine
writing bytes into a `u32`, means the returned `u32` *is* the register value. A memory card's ID
occupies the **low 16 bits**; §C.1 gives the encoding, and `__card_iscard()` rejects anything with
bits 31:16 set (`card.c:704`).

**4. `__card_iscard()`** (`card.c:699-717`) validates the word; `CARD_ProbeEx()` (`card.c:2453`)
reports `CARD_ERROR_WRONGDEVICE` if the check fails and `CARD_ERROR_READY` with the size and sector
size if it passes.

`CARD_ProbeEx()` also distinguishes "busy" from "no card" through `EXI_ProbeEx()` (`exi.c:712-717`),
which returns −1 — mapped to `CARD_ERROR_NOCARD` — only when the channel has never seen a device,
and 0 — mapped to `CARD_ERROR_BUSY` — when it has seen one but the debounce has not elapsed.

**The minimum an emulated slot must do to be seen at all: hold CSR bit 12 set, and answer the ID
command.** Everything else in the detection path is software state. The corollary is the useful
one: **a slot whose CSR reads zero is reported as empty and nothing hangs**, which is why the
runtime's current do-nothing EXI does not break games that offer to save.

One more constraint from the same code: `EXI_Deselect()` re-probes after every deselect
(`exi.c:424-430`) and `__card_txhandler()` re-probes after every completed transfer
(`card.c:975`). **Bit 12 must stay set for the whole session**; dropping it mid-operation is read
as the card being pulled out and turns into `CARD_ERROR_NOCARD`.

## A.4 What `runtimes/runtime-gc` does today

`runtimes/runtime-gc/src/gc_io.c:523-562` implements `exi_start()`, hooked onto the three control registers
(`gc_io.c:1051`, `1054`, `1057`). Its behaviour:

* Any transfer completes inside the write that started it, by clearing TSTART (`gc_io.c:561`).
* An immediate **read** stores zero into the data register (`gc_io.c:553`).
* An immediate **write** records the whole 32-bit data word in `exi_command[channel]`
  (`gc_io.c:556`).
* A **DMA read** zeroes the destination, and then, if the channel is 0 and the last recorded
  command word was `0x20000100`, overlays the 64-byte settings image (`gc_io.c:544-549`).
* A **DMA write** is discarded.
* `GC_INT_EXI` (`runtimes/runtime-gc/include/gc.h:52`) exists and is never asserted.

Three structural gaps stand between that and a card, none of them large but all of them load-bearing:

1. **There is no chip-select visibility.** The CSR at offset 0x00 is a plain storage register with
   no hook (`gc_io.c:1049`), so the runtime cannot see a device being selected or deselected. A card
   needs it, because a command is a *sequence* of transfers between one select and the next
   deselect: `0x52` plus four address bytes plus the latency bytes plus a DMA, all inside one
   select (§B.1). The fix is a write hook on the CSR that watches bits 9:7.
2. **`exi_command[]` keeps only the last immediate word, not a byte stream.** That is sufficient for
   the settings device, whose command is a single 4-byte word, and insufficient for the card, whose
   5-byte command arrives as a 4-byte immediate followed by a 1-byte immediate (§B.1) — the record
   would hold the fifth byte.
3. **DMA writes go nowhere,** so a page write could not reach an image.

None of this is criticism of the existing code; §A.3 explains why it is exactly right for a machine
that reports both slots empty.

---

# Part B — the command set

## B.1 Framing

A command is: **select device 0 → one or more immediate writes carrying the opcode and its address
bytes → optionally some latency bytes → optionally one data phase → deselect.** The data phase is
DMA for the two bulk commands and immediate for everything else.

Two framing details follow from `EXI_ImmEx()` (`exi.c:500-521`), which is how the multi-byte
commands are sent:

* **It splits into 4-byte immediate transfers**, any remainder last: `tc = nLen; if(tc>4) tc = 4;`. So
  the 5-byte read/write command reaches the wire as a 4-byte immediate then a 1-byte immediate.
  A device must concatenate them; the split is invisible on real hardware and is an artefact of the
  4-byte immediate register.
* **It synchronises after each piece** (`EXI_Sync()` inside the loop), so the pieces are strictly
  ordered and no interrupt-driven completion is involved for the command phase.

For the read command there is a third phase between the command and the data: **latency bytes**,
sent as `EXI_ImmEx(chn, card->workarea+CARD_READSIZE, card->latency, EXI_WRITE)` (`card.c:1518`,
and again in the retry path at `card.c:1060`). The buffer is scratch — the *contents* are
irrelevant, only the byte count is — and the count comes from the card's own ID (§C.1). For every
retail card that count is **4**, so the read command occupies **5 + 4 = 9 bytes** on the wire
before the data. *(Dolphin's memory-card device, read as a behaviour reference, also treats the
read command as nine bytes before the data begins. Two independent implementations agreeing is
good evidence that the latency bytes are part of the command and not something libogc invented.)*

## B.2 The commands

| Opcode | Name | Command phase | Latency | Data phase | Length | Evidence |
|---|---|---|---|---|---|---|
| `0x00 0x00` | **read ID** | imm write 2 | — | **imm read** | 4 bytes | `exi.c:611-616` (generic EXI, not card-specific) |
| `0x83 0x00` | **read status** | imm write 2 | — | **imm read** | 1 byte | `card.c:842-845` |
| `0x89` | **clear status** | imm write 1 | — | none | — | `card.c:869-871` |
| `0x81 0x01` / `0x81 0x00` | **enable / disable interrupt** | imm write 2 | — | none | — | `card.c:942-947` |
| `0x52 a₁ a₂ a₃ a₄` | **read segment** | imm write 5 | `latency` bytes | **DMA read** | **512** | `card.c:1504-1519` |
| `0xF2 a₁ a₂ a₃ a₄` | **write page** | imm write 5 | — | **DMA write** | **128** | `card.c:1472-1485` |
| `0xF1 a₁ a₂` | **sector erase** | imm write 3 | — | none | — | `card.c:1733-1745` |
| `0x87` | **wake** | imm write 1 | — | none | — | `card.c:917-919`, inside `#if 0` |
| `0x88` | **sleep** | imm write 1 | — | none | — | `card.c:893-895`, inside `#if 0` |

Three opcodes appear in libogc only as values it *tests for*, never as values it sends:
`__unlocked_callback()` has `if(card->cmd[0]==0xf3 || card->cmd[0]>=0xf5) return;` (`card.c:1403`)
and `__setuptimeout()` gives `0xf4` the same long timeout as the sector erase (`card.c:1019`).

**Unverified:** what `0xF3`, `0xF4` and `0xF5`+ do. The shared timeout makes `0xF4` an erase of some
kind, and Dolphin's device implements a chip erase and an "extra byte program" alongside the
page program, which fits — but no source read here establishes the mapping, and libogc never issues
them, so **an emulated card can reject them and no libogc-based software will notice.** What the
retail CARD library issues is a separate question; it would be settled by tracing a commercial
game's writes to the EXI immediate register during a save.

### The two bulk transfers in detail

**Read** (`ogc_card_readsegment()`, `card.c:1493-1528`) — one select, then:

1. `EXI_ImmEx` 5 bytes: `0x52`, then the four address bytes of §B.3;
2. `EXI_ImmEx` `latency` bytes of don't-care;
3. `EXI_Dma(chn, buf, CARD_READSIZE, EXI_READ, __card_txhandler)` — **512 bytes**
   (`card.h:54` defines `CARD_READSIZE` as 512).

Completion is the **transfer-complete interrupt**, which runs `__card_txhandler()` (`card.c:955`),
which deselects, unlocks and calls the read callback. A read therefore needs no card-generated
interrupt at all.

**Write** (`ogc_card_writepage()`, `card.c:1462-1490`) — one select, then:

1. `EXI_ImmEx` 5 bytes: `0xF2`, then the same four address bytes;
2. `EXI_Dma(chn, buf, 128, EXI_WRITE, __card_txhandler)` — **128 bytes, no latency bytes**.

Here `__card_txhandler` only tidies up the bus: `__card_start()` was called with a NULL transfer
callback and the real callback in the EXI slot (`__card_start(chn,NULL,callback)`, `card.c:1481`),
so **completion of the actual programming arrives later, as an EXI interrupt from the card** (§B.4).

**Sector erase** (`ogc_card_sectorerase()`, `card.c:1721-1751`) — one select, a 3-byte immediate
write, then an immediate deselect and unlock. **No data phase at all**, and completion again via
the card's EXI interrupt.

Larger transfers are built from these. `ogc_card_write()` (`card.c:1594`) sets
`cmd_blck_cnt = block_len>>7` and issues one `0xF2` per 128 bytes, advancing the address by 128 each
time (`__blockwritecallback()`, `card.c:1339-1345`). `ogc_card_read()` (`card.c:1615`) sets
`cmd_blck_cnt = block_len>>9` and issues one `0x52` per 512 bytes (`__blockreadcallback()`,
`card.c:1366-1372`). **An 8192-byte system block is therefore 16 read commands or 64 write
commands.**

## B.3 The address encoding

Both bulk commands pack a 24-bit byte address into four bytes, identically
(`card.c:1473-1476` and `card.c:1505-1508`):

```c
cmd[1] = (addr & 0xFE0000) >> 17;    /* bits 23:17 */
cmd[2] = (addr & 0x01FE00) >> 9;     /* bits 16:9  */
cmd[3] = (addr & 0x000180) >> 7;     /* bits  8:7  */
cmd[4] = (addr & 0x00007F);          /* bits  6:0  */
```

Read left to right, that is **a 15-bit segment number (bits 23:9), a 2-bit page-within-segment
(bits 8:7), and a 7-bit byte-within-page (bits 6:0)** — which is exactly the granularity of the two
commands: a segment is the 512 bytes a `0x52` returns, and a page is the 128 bytes an `0xF2`
programs, four pages to a segment.

The sector erase sends only the first two of those bytes (`card.c:1734-1735`):

```c
cmd[1] = (sector>>17)&0x7f;
cmd[2] = (sector>>9)&0xff;
```

so an erase addresses a 512-byte segment and the card erases the sector containing it. libogc
insists the address be sector-aligned before it sends anything: `if(sector%card->sector_size)
return CARD_ERROR_FATAL_ERROR;` (`card.c:1731`).

**The erase unit is the card's sector size (§C.1), not something larger.** This is arithmetic that
has to be true, not a claim from a comment: `__card_updatedir()` (`card.c:1861-1881`) erases the
single sector holding one directory block and then rewrites only that block. If an erase cleared
anything bigger than 8192 bytes it would take the header, the other directory copy, or a block
allocation table with it, and the ping-pong of §D.6 could not work. *(Dolphin's device carries a
comment about a 128 KB block; whatever that refers to, it cannot be the granularity libogc depends
on here.)*

## B.4 Completion, and why a write needs an interrupt

| Command | How libogc learns it finished |
|---|---|
| ID, read status, clear status, set interrupt | synchronously — `EXI_Sync()` spins on TSTART |
| read segment (`0x52`) | the EXI **transfer-complete** interrupt for the DMA |
| write page (`0xF2`) | the **card's own EXI interrupt**, after the DMA has completed |
| sector erase (`0xF1`) | the **card's own EXI interrupt**; there is no data phase to complete |

The card's interrupt is enabled once, at mount, by `__card_enableinterrupt(chn,1)` — command
`0x81 0x01` — and the handler is installed immediately after
(`EXI_RegisterEXICallback(chn,__card_exihandler)`, `card.c:1988-1989`).

`__card_exihandler()` (`card.c:1091-1130`) then does, on each such interrupt:

1. cancel the timeout alarm;
2. lock the channel and issue **read status** (`0x83 0x00`);
3. issue **clear status** (`0x89`);
4. `if(status&0x18) ret = CARD_ERROR_IOERROR;` (`card.c:1108`) — **bits 3 and 4 of the status byte
   are the error bits**; anything else is success;
5. on error, retry the whole command up to `cmd_retries` times (3 for a write or erase, 0 for a
   read — `card.c:1479`, `card.c:1738`, `card.c:1511`).

So the status byte is read twice per write and per erase, and its bits 4:3 must be clear.

**An emulated card that never raises the EXI interrupt cannot complete a write.** The write would
appear to work — the DMA finishes, the bus is released — and then nothing further would happen until
the watchdog below fired. This is the single most likely way to build a card that reads correctly
and silently fails to save.

Raising it means: set CSR bit 1 for the channel, and assert `GC_INT_EXI` through
`gc_set_interrupt()` the way `gc_io.c` already does for DI, SI, VI and the DSP. libogc acknowledges
by writing the bit back set (`__exi_clearirqs()`, `exi.c:108-119`), so the status bit must be
**sticky until acknowledged**, exactly like the VI's display interrupts
(`gc-video-out.md` §B.4).

## B.5 The timeouts

`__setuptimeout()` (`card.c:1011-1033`) arms a watchdog before each command that completes
asynchronously:

| Command | Timeout |
|---|---|
| `0xF1` sector erase, `0xF4` | `2 × (sector_size / 8192)` **seconds** — 2 s for a retail card |
| `0xF2` write page | **100 ms** |
| everything else | none |

On expiry, `__timeouthandler()` (`card.c:982-1009`) delivers `CARD_ERROR_IOERROR` to the pending
callback. These are worth knowing for two reasons: they are the failure mode a missing interrupt
produces (a save that fails 100 ms later, not a hang), and they are wall-clock generous enough that
an emulated card can complete instantly without any risk of being too fast — nothing in libogc
requires a minimum delay.

---

# Part C — the card's identity

## C.1 The ID word

The four bytes returned by the ID command are one 32-bit word with four fields packed into the low
16 bits. `__card_iscard()` (`card.c:699-717`) and `__card_domount()` (`card.c:1920-1938`) between
them decode it:

| Bits | Field | Decode |
|---|---|---|
| 31:16 | must be zero | `if(id&~0xffff) return 0;` (`card.c:704`) |
| 13:11 | **sector-size index** | `card_sector_size[(id>>11) & 7]` |
| 10:8 | **latency index** | `card_latency[(id>>8) & 7]` |
| 7:2 | **size in megabits** | `card->card_size = (id&0xfc)` (`card.c:1926`) |
| 1:0 | must be zero | `if(id&0x03) return 0;` (`card.c:705`) |

Bits **15:14** are constrained by nothing libogc does; it only requires the whole word to fit in 16
bits. **Unverified:** what, if anything, they carry. Zero is the safe answer for an emulated card.

**The two index fields are the easiest thing here to get wrong, so here is the derivation.** libogc
writes them as rotations:

```c
idx = _ROTL(id,23)&0x1c;  card->sector_size = card_sector_size[idx>>2];   // card.c:1931-1932
idx = _ROTL(id,26)&0x1c;  card->latency     = card_latency[idx>>2];       // card.c:1936-1937
```

`_ROTL(x,n)` is `((x)<<(n))|((x)>>(32-(n)))`. For `_ROTL(id,23)`, the mask `0x1c` selects bits 4:2
of the result; the `id<<23` half contributes nothing to bits 4:2 (it only reaches bits 23 and
above), so those three bits come entirely from `id>>9`, i.e. **bits 13:11 of `id`**. The same
argument for `_ROTL(id,26)` with `id>>6` gives **bits 10:8**. Then `idx>>2` undoes the ×4 the mask
left in place. Hence `(id>>11)&7` and `(id>>8)&7`. Note that the size field at bits 7:2 does not
overlap either of them.

The two tables (`card.c:184-206`):

| Index | `card_sector_size[]` | `card_latency[]` |
|---|---|---|
| 0 | 0x02000 = **8 KB** | **4** |
| 1 | 0x04000 = 16 KB | 8 |
| 2 | 0x08000 = 32 KB | 16 |
| 3 | 0x10000 = 64 KB | 32 |
| 4 | 0x20000 = 128 KB | 64 |
| 5 | 0x40000 = 256 KB | 128 |
| 6 | 0 — invalid | 256 |
| 7 | 0 — invalid | 512 |

A sector-size index of 6 or 7 makes `__card_iscard()` reject the card (`card.c:713`).

## C.2 Size in megabits, and the block count

`card->card_size = id & 0xfc` is a **megabit count**, and the accepted values are exactly the six
`EXI_MEMCARD*` constants of `exi.h:125-130`:

```c
#define EXI_MEMCARD59    0x00000004   /*   64 blocks /   4 Mbit / 0.5 MB */
#define EXI_MEMCARD123   0x00000008   /*  128 blocks /   8 Mbit / 1.0 MB */
#define EXI_MEMCARD251   0x00000010   /*  256 blocks /  16 Mbit / 2.0 MB */
#define EXI_MEMCARD507   0x00000020   /*  512 blocks /  32 Mbit / 4.0 MB */
#define EXI_MEMCARD1019  0x00000040   /* 1024 blocks /  64 Mbit / 8.0 MB */
#define EXI_MEMCARD2043  0x00000080   /* 2048 blocks / 128 Mbit / 16.0 MB */
```

The block count comes out of the size and the sector size (`card.c:1933`):

```c
card->blocks = ((card->card_size<<20)>>3)/card->sector_size;
```

— megabits × 2²⁰ ÷ 8 is the byte count, divided by the sector size. **With the 8 KB sector size that
index 0 selects, that is megabits × 16**, and the five system blocks (§D.1) come off the top:

| ID | Mbit | Bytes | Blocks | Usable | Name |
|---|---|---|---|---|---|
| `0x0004` | 4 | 524 288 | 64 | **59** | "Memory Card 59" |
| `0x0008` | 8 | 1 048 576 | 128 | **123** | |
| `0x0010` | 16 | 2 097 152 | 256 | **251** | "Memory Card 251" |
| `0x0020` | 32 | 4 194 304 | 512 | **507** | |
| `0x0040` | 64 | 8 388 608 | 1024 | **1019** | "Memory Card 1019" |
| `0x0080` | 128 | 16 777 216 | 2048 | **2043** | |

The "usable" column reproduces the numbers in libogc's own constant names, which is a useful check
that the arithmetic is the intended one: 64 − 5 = 59, 256 − 5 = 251, 2048 − 5 = 2043, and so on for
all six. **A "block" in the file system is one sector**, so on a retail card a block is 8 KB and a
save file's size is quoted in 8 KB blocks.

**Unverified:** which of these six were sold as products, and by whom. libogc's comments call all
six "Nintendo memory card", which is at best loose — Nintendo is widely reported to have sold only
the 59-, 251- and 1019-block cards, with the larger capacities coming from third parties, but
nothing read here establishes that. It does not matter for an emulated card, which only has to pick
one and answer consistently; **the 4 Mbit / 59-block card, ID `0x00000004`, is the conservative
choice** because it is the smallest valid size and the one whose sector-size and latency indices are
both zero.

**Unverified:** whether any real card reports a sector size other than 8 KB. The table allows up to
256 KB, and the derived block counts above match libogc's own constant names **only** at 8 KB: the
128 Mbit card gives 2048 blocks and 2043 usable with 8 KB sectors, but 1024 blocks and 1019 usable
with 16 KB sectors, which is the name of a different constant. The same holds at every capacity. So
all six of libogc's named cards use 8 KB sectors, and the larger sector sizes in the table are
unexercised by anything read here. **This matters for the on-card layout**: see §D.1.

`CARD_GetSerialNo()` (`card.c:3262-3279`) folds the header's serial area into two words by XOR —
`serial[0]^serial[2]^serial[4]^serial[6]` and `serial[1]^serial[3]^serial[5]^serial[7]` — which is
the only thing in libogc that reads the serial back, and nothing compares it to anything.

---

# Part D — the on-card layout

## D.1 The system area

The first **five blocks** of the card are reserved (`card.c:51`, `#define CARD_SYSAREA 5`). In card
address order:

| Block | Card address | Contents |
|---|---|---|
| 0 | `0` | **header** |
| 1 | `1 × sector_size` | **directory** |
| 2 | `2 × sector_size` | **directory backup** |
| 3 | `3 × sector_size` | **block allocation table (BAT)** |
| 4 | `4 × sector_size` | **BAT backup** |
| 5 … | `5 × sector_size` … | **file data** — the first block a file can occupy |

**Evidence.** `__card_domount()` reads them one at a time as `ogc_card_read(chn,
card->sector_size*(card->mount_step-2), 8192, card->workarea+((card->mount_step-2)<<13), …)`
(`card.c:1995`) for `mount_step` 2 through 6 (`card.c:2018`, `if((++card->mount_step)<7)`), so five
blocks at five consecutive multiples of the sector size, landing at five consecutive 8192-byte
offsets in the work area. `__card_formatregion()` writes the same five back (`card.c:1715` and
`__format_callback()`, `card.c:1299-1327`). `__card_allocblock()` will not hand out a block below
`CARD_SYSAREA` (`card.c:768`), and `__card_seek()` rejects a first block below it as
`CARD_ERROR_FATAL_ERROR` (`card.c:542`).

The work-area offsets — `CARD_SYSDIR 0x2000`, `CARD_SYSDIR_BACK 0x4000`, `CARD_SYSBAT 0x6000`,
`CARD_SYSBAT_BACK 0x8000` (`card.c:52-55`) — are **RAM offsets, not card addresses**, and the
`CARD_WORKAREA_SIZE` of `5*8*1024` (`card.h:53`) is what holds them. On a card with 8 KB sectors
the two coincide; **on a card with larger sectors they do not**, because libogc reads and writes
8192 bytes at each multiple of `sector_size`. On such a card each system structure would be 8192
bytes at the start of a larger block, with the remainder of the block unused.

**Unverified:** whether that is what real hardware and the retail library do on a card with sectors
larger than 8 KB, or whether libogc simply never meets one. Since §C.2 concludes that every named
card uses 8 KB sectors, the question may be vacuous. It is settled by finding any card whose ID has
a non-zero sector-size index.

## D.2 The header

`struct card_header` (`card.c:59-67`) is 512 bytes at the start of the 8192-byte header block; the
remaining 7680 bytes are filled with `0xFF` by the format and are covered by no checksum.

| Offset | Size | Field | What the format puts there |
|---|---|---|---|
| `0x00` | 12 | **serial** | 12 bytes derived from the SRAM flash ID for this slot plus a PRNG (§D.2.1) |
| `0x0C` | 8 | **format time** | the 64-bit value of `gettime()` — a Gekko timebase reading, not a date (`card.c:1681`) |
| `0x14` | 4 | **SRAM counter bias** | copied from the settings block's `counter_bias` (`card.c:1663`) |
| `0x18` | 4 | **SRAM language** | copied from the settings block's `lang`, a `u8` widened to a `u32` (`card.c:1664`) |
| `0x1C` | 4 | **VI DTV status** | `_viReg[55]`, the component-cable register of `gc-video-out.md` §B.1, sampled at format time (`card.c:1659`, `card.c:1682`) |
| `0x20` | 2 | **device ID** | `0` (`card.c:1683`) |
| `0x22` | 2 | **size**, in megabits | `card->card_size` — the same value the ID word carries (`card.c:1684`) |
| `0x24` | 2 | **encoding** | `SYS_GetFontEncoding()`: 0 = ANSI, 1 = Shift-JIS (`card.c:1660`, `card.c:2976`) |
| `0x26` | 470 | padding | `0xFF` |
| `0x1FC` | 2 | **chksum1** | sum over bytes `0x000`–`0x1FB` (§D.3) |
| `0x1FE` | 2 | **chksum2** | complement sum over the same range |

The struct declares the first 32 bytes as `u32 serial[0x08]`, which is why words 5, 6 and 7 of
"serial" are the bias, language and DTV fields: `header->serial[5] = sram->counter_bias;
header->serial[6] = sram->lang;` (`card.c:1663-1664`) and `header->serial[7] = tmp;`
(`card.c:1682`), with `*(u64*)&(header->serial[3]) = time;` (`card.c:1681`) covering words 3 and 4.
Only words 0–2 — the first 12 bytes — are the serial proper. All multi-byte fields are big-endian,
because the Gekko is and libogc writes them as native integers.

### D.2.1 The serial

`__card_formatregion()` builds it byte by byte (`card.c:1668-1680`):

```c
rnd_val = time = gettime();
while(cnt<12) {
    rnd_val = (((rnd_val*(u64)0x41c64e6d)+(u64)0x3039)>>16);
    ((u8*)header->serial)[cnt] = (sramex->flash_id[chn][cnt]+(u32)rnd_val);
    rnd_val = (((rnd_val*(u64)0x41c64e6d)+(u64)0x3039)>>16) & 0x7fff;
    cnt++;
}
```

so byte *i* is the *i*th byte of that slot's 12-byte SRAM flash ID plus a value from a linear
congruential generator seeded from the timebase. **Nothing anywhere in libogc checks this
relationship afterwards — but the retail CARD library does, on every mount** (its `VerifyID`,
read in a decompiled SDK as a behaviour reference): it re-runs the generator from the format time
at `0x0C` and calls the card broken unless every serial byte equals the console's flash ID plus the
generator's byte. This was found by Shrek 2 calling a card we formatted corrupted (GC-PLAN.md, "A
real game's saves"). An emulated card therefore needs its header and the settings block's flash ID
to agree: `runtimes/runtime-gc` recovers the flash ID from the header and reports it in the settings block,
which is what the unlock handshake achieves on hardware.

## D.3 The checksum

One routine covers all five system structures (`__card_checksum()`, `card.c:251-265`):

```c
*cs1 = 0; *cs2 = 0;
len /= 2;
for (i = 0; i < len; ++i) {
    *cs1 += buff[i];
    *cs2 += (buff[i] ^ 0xffff);
}
if (*cs1 == 0xffff) *cs1 = 0;
if (*cs2 == 0xffff) *cs2 = 0;
```

In words: **treat the region as big-endian 16-bit words; `cs1` is their sum and `cs2` is the sum of
their one's complements, both truncated to 16 bits; and a result of `0xFFFF` is stored as `0`
instead.** The `len` argument is a **byte** count, halved on entry.

That last substitution is not decoration — it is the only reason two all-`0xFF` directory blocks can
be valid. It also means an implementation that computes the sums and stops has a one-in-65536 chance
per structure of writing a checksum libogc will reject.

The three regions:

| Structure | Region checksummed | Bytes | Where the pair is stored |
|---|---|---|---|
| **header** | offset `0x000` … `0x1FB` | 508 | `0x1FC`, `0x1FE` |
| **directory** | offset `0x0000` … `0x1FFB` | 8188 | `0x1FFC`, `0x1FFE` |
| **BAT** | offset `0x0004` … `0x1FFF` | 8188 | `0x0000`, `0x0002` |

**Evidence.** Header: `__card_checksum((u16*)header,508,&header->chksum1,&header->chksum2)`
(`card.c:1685`), with the struct putting the pair immediately after 508 bytes. Directory:
`__card_checksum((u16*)dirblock[dir],0x1ffc,&chksum0,&chksum1)` in `__card_checkdir()`
(`card.c:600`) — `0x1ffc` is 8188 — compared against the pair in the control block at the end.
BAT: `__card_checksum((u16*)(((u32)fatblock[fat])+4),0x1ffc,&chksum0,&chksum1)` in
`__card_checkfat()` (`card.c:641`) — the same 8188 bytes, but starting **four bytes in**, because
the BAT keeps its checksums at the *front*. The two blocks differ in this and it is easy to
transpose: **the directory's checksums are at the end and cover everything before them; the BAT's
are at the start and cover everything after them.** Both regions are 8188 bytes, and in both cases
the four bytes holding the pair are excluded.

## D.4 The directory block

8192 bytes: **127 entries of 64 bytes each** (8128 bytes), followed by a 64-byte control block.

```c
struct card_dat     { card_direntry entries[CARD_MAXFILES]; };   // card.c:69-71
struct card_dircntrl{ u8 pad[58]; u16 updated; u16 chksum1; u16 chksum2; };  // card.c:73-78
```

with `CARD_MAXFILES` = 127 (`card.h:56`) and the control block located at `dirblock+8128`
(`card.c:599`, `card.c:1875`). The arithmetic closes exactly: 127 × 64 = 8128, plus 58 + 2 + 2 + 2 =
64, is 8192.

| Offset in block | Size | Field |
|---|---|---|
| `0x0000` | 8128 | 127 directory entries (§E) |
| `0x1FC0` | 58 | padding, `0xFF` after a format |
| `0x1FFA` | 2 | **updated** — the generation counter |
| `0x1FFC` | 2 | chksum1 |
| `0x1FFE` | 2 | chksum2 |

A **free entry is marked by `gamecode[0] == 0xFF`** — that is the test everywhere: `__card_getfilenum()`
skips entries whose first game-code byte is `0xFF` (`card.c:512`), `CARD_CreateAsync()` takes the
first such entry as the slot for a new file (`card.c:2685`), and `__card_seek()` refuses to operate
on one (`card.c:552`). Since a formatted directory is all `0xFF`, all 127 entries start free.

## D.5 The block allocation table

8192 bytes (`card.c:80-87`):

| Offset | Size | Field |
|---|---|---|
| `0x0000` | 2 | chksum1 |
| `0x0002` | 2 | chksum2 |
| `0x0004` | 2 | **updated** — the generation counter |
| `0x0006` | 2 | **freeblocks** |
| `0x0008` | 2 | **lastalloc** — the block the last allocation ended on |
| `0x000A` | 8182 | **fat[0xFFB]** — 4091 entries of `u16` |

4091 × 2 = 8182, plus the 10-byte head, is 8192.

**`fat[i]` describes block `i + CARD_SYSAREA`**, i.e. entry 0 is block 5, the first data block. The
bias appears in every access: `fatblock->fat[currblock-CARD_SYSAREA]` in `__card_allocblock()`
(`card.c:769`), `fatblock->fat[block-CARD_SYSAREA]` in `__card_freeblock()` (`card.c:797`), and
`fatblock->fat[i-CARD_SYSAREA]` when walking a chain in `__card_seek()` (`card.c:573`).

| Value | Meaning |
|---|---|
| `0x0000` | **free** — `__card_checkfat()` counts these as the free blocks (`card.c:653`), and `__card_allocblock()` takes them (`card.c:769`) |
| `0xFFFF` | **end of chain** — written to the last block of a new file (`card.c:775`) |
| anything else | the **next block number** in the file's chain (an absolute block number, not an index) |

`freeblocks` is validated at mount: `__card_checkfat()` walks blocks `CARD_SYSAREA` … `blocks-1`,
counts the zero entries, and marks the BAT bad if the count disagrees with the stored value
(`card.c:650-662`). **A BAT with a correct checksum but a wrong free-block count is rejected**, so
an image we write has to get both right.

`lastalloc` is only a search hint — `__card_allocblock()` starts scanning after it and wraps back to
`CARD_SYSAREA` (`card.c:767-768`) — so any in-range value is safe.

## D.6 Choosing between the two copies

Both the directory and the BAT exist twice, and the selection rule is the same for both
(`__card_checkdir()`, `card.c:585-624`; `__card_checkfat()`, `card.c:628-683`). Each returns the
number of *bad* copies it found and leaves `curr_dir` / `curr_fat` pointing into the work area.

1. **Check both copies.** A directory copy is bad if its checksum pair does not match; a BAT copy is
   bad if its checksum pair does not match *or* its `freeblocks` disagrees with the table.
2. **If neither is bad, pick by the generation counter**:
   ```c
   if(dircntrl[0]->updated<dircntrl[1]->updated) dir = 0; else dir = 1;
   ```
   **Read carefully: the index this selects is the copy to be *overwritten*, not the copy to be
   used.** The next two lines copy the *other* one over it:
   ```c
   card->curr_dir = dirblock[dir];
   memcpy(dirblock[dir],dirblock[dir^1],8192);
   ```
   So the newer copy's contents survive, in the older copy's buffer, and the next update will write
   to the older copy's physical block. **The copy with the larger `updated` is the current one.**
3. **If exactly one is bad, that one is the target** (`dir = bad_dir`), and the good one is copied
   over it — the same statement, reached differently.
4. **If both are bad**, `bad_dir` holds the second one found, and the corrupt other copy is copied
   over it. `__card_verify()` (`card.c:684-697`) then accepts the card anyway as long as
   `checkdir + checkfat <= 2` and both pointers are non-NULL. **libogc will mount a card whose
   directory failed both checksums.** That is a libogc property, not a hardware one, and §F.4
   returns to it.

The comparison is a plain unsigned `<` on a 16-bit counter, so it does not handle wraparound —
after 65536 updates the "newer" copy reads as older. Nothing in libogc resets the counter. This is
harmless in practice and worth not reproducing as a deliberate feature.

## D.7 How an update reaches the card

Directory and BAT updates follow the same sequence
(`__card_updatedir()`, `card.c:1861-1881`; `__card_updatefat()`, `card.c:1842-1859`):

1. **increment `updated`** and recompute the checksum pair over the block in RAM;
2. **`0xF1` erase** the sector holding that copy — the address is computed from the work-area offset
   as `(((u32)block-(u32)workarea)>>13)*card->sector_size`, i.e. which of the five system blocks
   this is, times the sector size;
3. on the erase-complete interrupt, **`ogc_card_write()` the whole 8192 bytes** — 64 page writes
   (`__card_faterasecallback()`, `card.c:1765`; `__card_direrasecallback()`, `card.c:1788`);
4. on completion, **swap the working pointer to the other copy** and copy the new contents into it
   (`__card_dirwritecallback()`, `card.c:1573-1580`; `__card_fatwritecallback()`, `card.c:1541`),
   so the next update lands on the block just superseded.

File data follows the same erase-then-write shape: `CARD_WriteAsync()` (`card.c:2629-2650`) rejects
any length or offset that is not a multiple of the sector size, then erases the file's current block
and writes it whole. **There is no partial-sector write path**; a card image is only ever modified a
whole sector at a time, which makes a file-backed image straightforward.

---

# Part E — a directory entry

64 bytes. `card.h:177-193` declares it as a struct, which is the best available evidence for the
field sizes because a compiler has to lay it out exactly this way — every field is naturally
aligned and there is no padding to insert.

| Offset | Size | Field | Notes |
|---|---|---|---|
| `0x00` | 4 | **`gamecode[4]`** | the disc's four-character game code. `0xFF` in byte 0 marks the entry free |
| `0x04` | 2 | **`company[2]`** | the two-character maker code |
| `0x06` | 1 | `pad_00` | "always 0xFF" per `card.h:163` |
| `0x07` | 1 | **`banner_fmt`** | bits 1:0 = `CARD_BANNER_NONE` / `_CI` / `_RGB` (`card.h:92-95`) |
| `0x08` | 32 | **`filename[32]`** | `CARD_FILENAMELEN` (`card.h:55`); zero-padded, and `CARD_CreateAsync()` copies `len+1` bytes over a zeroed field (`card.c:2712-2713`) |
| `0x28` | 4 | **`last_modified`** | seconds; `card.h:167` says "since 1970" and `card.h:215` says "since 2000" for the same quantity in `card_stat` — see below |
| `0x2C` | 4 | **`icon_addr`** | byte offset **within the file** of the icon image data |
| `0x30` | 2 | **`icon_fmt`** | two bits per icon, 8 icons: `CARD_ICON_NONE`/`_CI`/`_RGB` (`card.h:101-103`), packed by `CARD_GetIconFmt(stat,n)` as `(icon_fmt>>(2*n))&3` (`card.h:234`) |
| `0x32` | 2 | **`icon_speed`** | two bits per icon: `CARD_SPEED_END`/`_FAST`/`_MIDDLE`/`_SLOW` (`card.h:110-113`), same packing |
| `0x34` | 1 | **`permission`** | `CARD_ATTRIB_PUBLIC 0x04`, `_NOCOPY 0x08`, `_NOMOVE 0x10` (`card.h:84-86`) |
| `0x35` | 1 | **`copy_times`** | how many times the save has been copied |
| `0x36` | 2 | **`block`** | the **first block** of the file — an absolute block number, ≥ 5 |
| `0x38` | 2 | **`length`** | the file's size **in blocks** |
| `0x3A` | 2 | `pad_01` | "always 0xFFFF" per `card.h:174` |
| `0x3C` | 4 | **`comment_addr`** | byte offset within the file of the two 32-byte comment strings |

The offsets are the running sum of the sizes, which is the check that matters: 4+2+1+1+32+4+4+2+2+
1+1+2+2+2+4 = **64**, and 127 × 64 = 8128 leaves exactly the 64 bytes §D.4 needs for the control
block. If any field were a different width the directory would not fit its block.

Three notes an implementation needs:

* **`block` and `length` are block units, not bytes.** `__card_seek()` computes the file length as
  `entry->length*card->sector_size` (`card.c:553`) and reads at `file->iblock*card->sector_size`
  (`card.c:2612`). `CARD_CreateAsync()` sets `entry->length = size/card->sector_size` after
  rejecting any size that is not a multiple of it (`card.c:2678`, `card.c:2711`).
* **`icon_addr` and `comment_addr` are offsets into the save file's own data**, not card addresses.
  `__card_updateiconoffsets()` (`card.c:341`) walks them to build the `card_stat` offsets the API
  hands back. Nothing in the card's own bookkeeping dereferences them, so a card implementation can
  treat them as opaque.
* **The epoch of `last_modified` is not settled by these sources.** `card.h:167` documents the
  directory-entry field as seconds since 1970, while `card.h:215` documents the corresponding
  `card_stat` field as "time since 2000 in seconds", and `card.c` neither sets nor converts it —
  `__card_getstatusex()` copies the entry out verbatim (`card.c:412-430`). **Unverified:** which it
  is. It is settled by reading a card image written by a retail game and seeing which epoch puts the
  timestamp in this decade. It does not affect mounting; it affects only what a save browser
  displays.

---

# Part F — formatting

## F.1 What a formatted card contains

`__card_formatregion()` (`card.c:1634-1717`) builds all five system blocks in the work area and then
writes them. `__format_callback()` (`card.c:1299-1327`) drives the card side: **erase sectors 0
through 4 one at a time, then write the five 8192-byte blocks** (steps 5 through 9), then adopt the
first directory and BAT copies as current.

The five blocks, as the format leaves them:

**Block 0 — header.** `memset(header,0xff,8192)` first (`card.c:1657`), then the fields of §D.2
written over the front, then the checksum pair over bytes `0x000`–`0x1FB`. Everything from `0x200`
to the end of the block stays `0xFF`.

**Blocks 1 and 2 — directory and backup** (`card.c:1687-1694`):

```c
memblock = workarea+((cnt+1)<<13);
dircntrl = memblock+8128;
memset(memblock,0xff,8192);
__card_checksum(memblock,8188,&dircntrl->chksum1,&dircntrl->chksum2);
```

So **both copies are identical: 8188 bytes of `0xFF` — including `updated`, which is left at
`0xFFFF` — followed by the checksum pair.** Every one of the 127 entries has
`gamecode[0] == 0xFF` and is therefore free. The checksum of 4094 words of `0xFFFF`: since
`0xFFFF ≡ −1 (mod 2¹⁶)`, `cs1` is `−4094 mod 2¹⁶` = **`0xF002`**, and `cs2` is a sum of
`0xFFFF ^ 0xFFFF` = **`0x0000`**. (Worth computing once and asserting in a test; a format that gets
it wrong fails at the next mount, not at format time.)

Both copies having `updated == 0xFFFF` means the rule of §D.6 takes its `else` branch and picks
copy 1 as the one to overwrite, which is harmless — the two are identical.

**Blocks 3 and 4 — BAT and backup** (`card.c:1696-1704`):

```c
memblock = workarea+((cnt+3)<<13);
fatblock = memblock;
memset(memblock,0,8192);
fatblock->updated = cnt;                              /* 0 and 1 */
fatblock->freeblocks = card->blocks-CARD_SYSAREA;
fatblock->lastalloc = 4;
__card_checksum(memblock+4,8188,&fatblock->chksum1,&fatblock->chksum2);
```

**Zero-filled, not `0xFF`-filled** — the opposite of the directory, because a zero FAT entry means
"free". `updated` is 0 in the first copy and 1 in the second, so the second is the newer and the
first is the one the next update will overwrite. `freeblocks` is the whole card less the system
area — 59 for a 4 Mbit card — and `lastalloc` is 4, the last system block, so the first allocation
scan starts at block 5.

## F.2 The two gates outside the card

A correctly formatted image is necessary and not sufficient. `__card_domount()` (`card.c:1904-2003`)
puts two more conditions between the ID read and the first block read.

**Gate one: the card must report itself unlocked.** After validating the ID it issues clear-status
and read-status, and then (`card.c:1946`):

```c
if(!(status&CARD_STATUS_UNLOCKED)) {   /* 0x40, card.c:57 */
    if((ret=__dounlock(chn,card->key))<0) goto exit;
    ...
}
```

`__dounlock()` (`card.c:2266` onward) is a challenge-response built on a shift-register cipher
(`exnor()`, `exnor_1st()`, `bitrev()`), driven by **a DSP task** (`card.c:2179`, and the
`_cardunlockdata[0x160]` microcode blob at `card.c:129`/`card.c:156`), using `0x52` read commands with a
*different* address packing (`ogc_card_readarrayunlock()`, `card.c:2142-2177`). **An emulated card
should report bit 6 set and never be asked to do any of this.** If bit 6 is clear, the whole DSP
path runs, and it ends by re-reading the status and returning `CARD_ERROR_IOERROR` if bit 6 is
still clear (`card.c:2259-2262`) — so a card that always reports locked fails to mount rather than
hanging, but it fails after an enormous amount of pointless machinery.

**Gate two: the SRAM flash-ID checksum must be consistent.** If the card *is* unlocked, libogc
takes the other branch (`card.c:1968-1979`):

```c
cnt = 0; sum = 0;
sramex = __SYS_LockSramEx();
while(cnt<12) { sum += sramex->flash_id[chn][cnt]; cnt++; }
cnt = sramex->flashID_chksum[chn];
__SYS_UnlockSramEx(0);

sum = (sum^-1)&0xff;
if(cnt!=sum) { ret = CARD_ERROR_IOERROR; goto exit; }
```

**The mount fails with `CARD_ERROR_IOERROR` unless `flashID_chksum[slot]` equals the one's
complement of the byte-wise sum of `flash_id[slot][0..11]`, truncated to 8 bits.**

That is a condition on the **console's 64-byte settings block**, not on the card. In the layout
`system.h:184-219` declares — `syssram` is 20 bytes, `syssramex` the remaining 44 — the relevant
offsets within the 64 bytes are:

| Offset | Field |
|---|---|
| `0x14`–`0x1F` | `flash_id[0]` — slot A's 12-byte flash ID |
| `0x20`–`0x2B` | `flash_id[1]` — slot B's |
| `0x3A` | `flashID_chksum[0]` |
| `0x3B` | `flashID_chksum[1]` |

**This is a live bug in the runtime as it stands.** `runtimes/runtime-gc/src/gc_io.c:501-516` builds a
settings image that is zero everywhere except bytes `0x10`–`0x13` and the checksum pair. With
`flash_id[0]` all zero, the required checksum is `(~0) & 0xFF = 0xFF`, and the image supplies `0x00`
— so the first libogc-based program to mount an already-unlocked card in slot A would fail with
`CARD_ERROR_IOERROR` before reading a single block. **Bytes `0x3A` and `0x3B` of the settings image
must be `0xFF`** (given all-zero flash IDs), and the settings checksum recomputed afterwards. It
costs two bytes and is invisible until the day a card is implemented.

The same code path *writes* the SRAM when it has to unlock a card (`card.c:1954-1963`) — it stores
the key it derived as the flash ID and writes the matching checksum — which is why the two are
expected to agree.

## F.3 Adjacent: two things about the settings block

Both are outside this document's subject and both are in the 64 bytes the card's mount path reads,
so they are recorded here rather than lost.

**The settings checksum range is disputed.** `runtimes/runtime-gc/src/gc_io.c:490-499` computes the sum and
complement-sum over **bytes `0x04`–`0x3F`** — everything after the pair — and `docs/GC-PLAN.md`
describes the rule the same way, on the evidence that a commercial game stopped spinning once it was
implemented. libogc computes it over **four halfwords only, bytes `0x0C`–`0x13`**:

```c
static void __buildchecksum(u16 *buffer,u16 *c1,u16 *c2)   // system.c:596-607
{
    for(i=0;i<4;i++) { *c1 += buffer[6+i]; *c2 += buffer[6+i]^-1; }
}
```

*(Dolphin, as a behaviour reference, uses the narrow range too — from the clock bias to the end of
the base settings structure.)* libogc never *verifies* the checksum on read (`__sram_init()`,
`system.c:585-594`, reads the block and checks nothing), so libogc-based software cannot tell the
two apart; only the retail SDK can. For the current image the two rules happen to agree on the first
sum (`0x0040` either way) and disagree on the second (`0xFFA2` wide, `0xFFBC` narrow), so the
observation that a game proceeded is evidence for the wide rule **only if that game verifies the
second word**. **Unverified:** which range the retail SDK uses. It is settled cheaply — write the
narrow-range values instead and see whether the same game still boots.

**The flags byte at `0x13` looks wrong.** `gc_io.c:510` writes `0x40` with the comment "stereo
sound, no progressive scan". libogc names the bits (`system.c:74-80`): bits 1:0 video mode, bit 2
sound mode, bit 3 OOBE-done, bit 6 boot-to-IPL, bit 7 progressive scan, with `SYS_GetSoundMode()`
reading bit 2 (`system.c:1633-1641`) and `SYS_GetProgressiveScan()` reading bit 7
(`system.c:1608-1616`). Under those names `0x40` sets *boot into the IPL menu* and leaves the sound
mode at mono; "stereo, no progressive" would be `0x04`. The `gc_io.c` comment already marks the
flags byte **Unverified:**, and this note is only to say that libogc's headers now give a specific
alternative reading to test against.

## F.4 What the retail library may check that libogc does not

Three gaps, in the order they would bite. All three are the same shape: libogc is permissive, and a
commercial game links a different library. **A fourth, which did bite:** the retail library checks
the header serial against the settings block's flash ID and the format time (§D.2.1); settled
2026-09-15 by Shrek 2.

1. **The header checksum.** `__card_verify()` (`card.c:684-697`) calls `__card_checkdir()` and
   `__card_checkfat()` and **never looks at the header at all** — no libogc code path validates
   bytes `0x1FC`/`0x1FE`. **Unverified:** whether the retail CARD library validates it and refuses to
   mount. Settled by mounting an image with a deliberately wrong header checksum under a commercial
   game. **Write it correctly regardless**; it costs nothing.
2. **Two bad directory copies.** §D.6 step 4: libogc mounts a card whose directory failed both
   checksums, because `__card_verify()` accepts any total below 3. **Unverified:** whether the
   retail library does. Assume it does not.
3. **The encoding field.** `card.h:76` defines `CARD_ERROR_ENCODING` as "font encoding PAL/SJIS
   mismatch", and **no code in `card.c` ever returns it** — libogc writes `encoding` at format time
   and never compares it to anything afterwards. The error code exists because the retail library
   has it. **Unverified:** what it compares against; presumably the console's own font encoding, as
   `CARD_Format()` uses `SYS_GetFontEncoding()` for the value it writes (`card.c:2976`). **Write
   `0` (ANSI) for a non-Japanese console**, matching what our settings block reports.

---

# Part G — the smallest thing that would let a game save and load

Ordered so that each step is testable before the next is written.

**1. Make one slot visible.** Hold **CSR bit 12 set** for channel 0, permanently. That is the whole
of "a card is in slot A" as far as detection goes (§A.3). Leave channel 1's CSR at zero and slot B
stays empty, which is correct and halves the work.

**2. See chip-select.** Add a write hook to the channel-0 CSR (`gc_io.c:1049` currently declares it
with none) that tracks bits 9:7, and start a fresh command byte-stream whenever device 0 goes from
deselected to selected. Everything in Part B is framed by this.

**3. Answer the ID.** Two zero bytes in, four bytes out, with the value in the **low 16 bits of the
immediate register** and the first wire byte in bits 31:24 (§A.2). Use **`0x00000004`** — a 4 Mbit,
59-block, 8 KB-sector, 4-latency-byte card (§C.2). The image is then 524 288 bytes.

**4. Answer status.** `0x83 0x00` then a one-byte immediate read returning a value with **bit 6 set**
(unlocked, §F.2) and **bits 4:3 clear** (no error, §B.4). Those are the only bits libogc looks at,
so `0x40` is sufficient; `0x41` also sets what Dolphin calls the ready bit, which costs nothing and
may matter to the retail library. Accept `0x89` and `0x81 xx` and do nothing with them. This is the point at which a game
will already say "memory card in slot A" and offer to load.

**5. Back it with a file.** A 524 288-byte image, memory-mapped or read whole at startup. Implement
`0x52` (§B.3 address decode, discard the four latency bytes, DMA 512 bytes out) and `0xF2` (DMA 128
bytes in) and `0xF1` (fill the containing 8192-byte sector with `0xFF`). The runtime's DMA-write
path does not exist yet (§A.4) and has to be added.

**6. Raise the interrupt.** After a `0xF2` and after an `0xF1`, set CSR bit 1 and assert
`GC_INT_EXI` (`gc.h:52`), sticky until libogc writes the bit back (§B.4). **Without this, loading
works and saving silently does not** — the write appears to succeed and then times out 100 ms later
as `CARD_ERROR_IOERROR`.

**7. Ship a formatted image.** Five system blocks per §F.1, so that a new user is not asked to
format. The three checksums of §D.3, the `freeblocks` count of §D.5, and the `0xFF`-versus-zero
distinction between the directory and the BAT are the four things to get right; everything else in
the header can be plausible rather than correct.

**8. Fix the two settings bytes.** `0x3A` and `0x3B` of the settings image to `0xFF`, and recompute
the settings checksum (§F.2). Two bytes, and without them the mount fails after everything above is
working, which is a miserable thing to debug.

That is enough for `CARD_Mount`, `CARD_Open`, `CARD_Read`, `CARD_Create` and `CARD_Write` to work
end to end.

## What can wait

* **The unlock cipher** (§F.2) — report unlocked and none of it is ever reached.
* **`0x87`, `0x88`, `0xF3`, `0xF4`, `0xF5`+** — libogc never issues them (§B.2); reject or ignore.
* **Timing realism.** Nothing requires a minimum delay; the watchdogs are 100 ms and 2 s (§B.5).
  Completing instantly is safe, and the busy bit (`0x80`, per Dolphin) need never be set.
* **The latency bytes as data.** They are writes into the void; only the count is part of the
  protocol, and the count is fixed by the ID we chose.
* **Program and erase error reporting.** Bits 4:3 of the status byte can stay clear for ever until
  there is a reason to simulate a failing card.
* **Insertion and removal.** CSR bit 11, the EXT interrupt, `EXI_Attach`/`EXI_Detach` and the
  detach callback a game passes to `CARD_Mount`. A card that is present from boot and never leaves
  needs none of it.
* **Slot B**, and any card size other than 4 Mbit.
* **Sector sizes other than 8 KB** (§D.1) — possibly vacuous.
* **The serial** (§D.2.1) — nothing reads it back except `CARD_GetSerialNo()`, which compares it to
  nothing.
* **Banner and icon data** — that is file content the game writes and reads through `CARD_Write`;
  the card layer never interprets it.

---

# What could not be determined

Recorded as **Unverified:** above, in descending order of how much it matters for getting a save to
work:

1. **Whether the retail CARD library validates the header checksum** (§F.4) where libogc does not.
   The cheapest insurance is to write it correctly; the cost of being wrong is a card the SDK offers
   to format. Settled by a commercial game against a deliberately-corrupted header.
2. **Whether the retail library accepts a card with two bad directory copies** (§D.6, §F.4). libogc
   does; assume nothing else does.
3. **Which byte range the SRAM settings checksum covers** (§F.3) — `0x04`–`0x3F` as the runtime
   implements it, or `0x0C`–`0x13` as libogc and Dolphin compute it. This is adjacent to the card
   but the card's mount path depends on the same block being trusted. Settled by one experiment.
4. **What commands `0xF3`, `0xF4` and `0xF5`+ do** (§B.2). libogc reserves them and never issues
   them; `0xF4` shares the erase timeout. Irrelevant unless the retail library issues them, which a
   trace of a commercial save would show.
5. **The epoch of a directory entry's `last_modified`** (§E) — `card.h` documents 1970 in one place
   and 2000 in another for the same quantity, and `card.c` never touches it. Affects only display.
6. **What the encoding field is compared against** (§F.4), given that `CARD_ERROR_ENCODING` exists
   and libogc never returns it.
7. **The meanings of status bits other than 6 (unlocked) and 4:3 (error)** (§B.4). Dolphin's device
   documents busy, sleep, separate erase and program errors, and ready — a second implementation,
   not hardware, and libogc tests none of them.
8. **Which of the six card sizes were real products, and whether any real card uses a sector size
   other than 8 KB** (§C.2, §D.1). The derived block counts say all six named cards use 8 KB
   sectors; the larger sector sizes in libogc's table are unexercised.
9. **What an erased byte reads as.** `0xFF` is the flash convention and is what §G step 5 assumes,
   but nothing in libogc can observe it: every erase is followed by a write of the whole sector
   (§D.7), so the erased value is never read back.
10. **The flags byte of the settings block** (§F.3). Already marked unverified in `gc_io.c`; libogc
    now supplies a specific alternative reading to test.
11. **Bits 15:14 of the card ID word** (§C.1). libogc constrains only bits 13:0 and the upper half.
    Zero is the safe answer.
12. **CSR bit 13** (§A.2). `__exi_init()` writes `0x2000` into channel 0's CSR once and nothing reads
    it back. Conventionally the boot-ROM disable; no source read here says so, and no card behaviour
    depends on it.

One libogc property documented above must **not** be reproduced as though it were hardware
behaviour: the generation-counter comparison in `__card_checkdir()` / `__card_checkfat()`
(`card.c:614`, `card.c:670`) is a plain unsigned `<` with no wraparound handling, and the
`tmp>8` sanity test in `__card_iscard()` (`card.c:715`) omits the divide-by-8 that
`__card_domount()` applies to the same expression (`card.c:1933`) — it is a loose bound that
happens to pass, not a specification of anything.
