# The GameCube DSP: the task protocol and the AX mixing model

This document is written for an implementation that intends to **emulate the DSP at a high level** —
to answer the CPU's task protocol and produce mixed audio directly, without ever executing a single
DSP instruction. It is therefore organised around the distinction that matters most for that plan:

* **Part A** is the *task protocol*. It is a CPU-side contract, spoken entirely through two mailbox
  registers and one interrupt. It is fixed across microcodes, it is small, and it is what a game's
  start-up actually waits on. An implementation that gets Part A right and does nothing else will
  boot a commercial SDK game with silence.
* **Parts B–D** are what the **AX microcode** does once a task is running: walk a command list,
  mix a linked list of voices, and hand a block of interleaved stereo samples to the audio
  interface's DMA. None of this is needed to satisfy Part A.

The most useful single sentence in this document: **a task is a microcode image plus an entry
vector, and the protocol that runs it says nothing about audio at all.** The same five mails carry
the memory-card microcode and the GBA-link microcode. Audio is what *AX* does inside a task; it is
not what a task is.

## Provenance

Five sources, all read directly. Nothing was copied from a GPL source into this document or into
this repository — the Dolphin material below was read as a *behaviour reference* and is restated in
prose.

1. **devkitPro/libogc `dsp.c`** (zlib licence), fetched from
   `https://raw.githubusercontent.com/devkitPro/libogc/master/libogc/dsp.c` (511 lines; line
   numbers below refer to that revision). This is the **primary authority for Part A**. libogc's
   DSP driver is a reimplementation of the Nintendo SDK's, mail constant for mail constant, and it
   is freely quotable. Where this document shows code, it is from here or from `audio.c`.
2. **devkitPro/libogc `audio.c`** (zlib), same revision (423 lines). The authority for the audio
   DMA registers (§D.1) and the audio interface's own register block (§D.3).
3. **libogc headers** `D:\devkitPro\libogc\include\ogc\{dsp.h, audio.h}` and
   `D:\devkitPro\libogc\include\tuxedo\interrupt.h`, for the task-state enumeration, the API
   surface and the interrupt identifiers.
4. **Dolphin's DSP-HLE** (GPL-2.0) — `UCodes/{UCodes.h, AX.h, AX.cpp, AXStructs.h, AXVoice.h,
   ROM.cpp, INIT.cpp}`, `DSP/DSPAccelerator.{h,cpp}`, `HW/{DSP.h, DSP.cpp, AudioInterface.cpp}`,
   fetched from the project's master branch. Read as a behaviour reference for everything libogc
   cannot show: what the *DSP* side of the protocol does, the AX command numbering, the parameter
   block layout, and the sample decoder. Dolphin is a second implementation, not hardware; claims
   that rest on it alone are marked as such.
5. **`runtime-gc/src/gc_io.c`** in this repository, for what already exists. The DSP block is
   `dsp_regs[]` at `gc_io.c:993`; the mailbox and control handling is `gc_io.c:60-291`; the audio
   interface is `gc_io.c:293-359` and `ai_regs[]` at `gc_io.c:1062`.

Register naming: the DSP interface registers have no names in libogc, which addresses them as a
bare `vu16` array (`dsp.c:62`: `static vu16* const _dspReg = (u16*)0xCC005000;`). The names below
are the conventional community ones; the **offsets and bit positions are derived from libogc's
array indices and from its own `DSPCR_*` constants**, not from the naming convention.

Anything neither libogc nor Dolphin pins down is marked **Unverified:** with a note on what would
settle it.

---

# Part A — the task protocol

## A.1 What a task is, and what it is not

At the driver level a **task** is the structure `dsptask_t` (`ogc/dsp.h:115-138`). Its substantive
fields are:

| Field | Meaning |
|---|---|
| `iram_maddr`, `iram_len`, `iram_addr` | The microcode image: where it lives in main memory, how long it is, and where in the DSP's instruction memory it goes |
| `dram_maddr`, `dram_len`, `dram_addr` | The microcode's data image, the same three ways |
| `init_vec`, `resume_vec` | Two entry points in DSP instruction space — one for a cold start, one for resuming after a yield |
| `prio` | Priority; lower sorts earlier (`dsp.c:79`, `if(task->prio<t->prio)`) |
| `state` | One of `DSPTASK_INIT`/`RUN`/`YIELD`/`DONE` = 0/1/2/3 (`ogc/dsp.h:47-50`) |
| `init_cb`, `res_cb`, `done_cb`, `req_cb` | Four CPU-side callbacks, one per protocol event |

Note what is **absent**: no sample rate, no buffer, no voice count, no mention of audio. A task is a
code image and an entry point. The driver maintains a priority-ordered doubly linked list of them
(`__dsp_inserttask`, `dsp.c:64-99`) and multiplexes the single DSP between them.

The four task states and the four callbacks line up exactly with four of the five mails, and the
correspondence is not a coincidence: **the low nibble of the mail is the state it puts the task
into.** `0xDCD10000` → `DSPTASK_INIT` handling, `0xDCD10001` → running again, `0xDCD10002` →
`DSPTASK_YIELD`, `0xDCD10003` → `DSPTASK_DONE`. That is a useful mnemonic and also a hint that the
protocol was designed around the state machine rather than around audio.

**The consequence for a high-level implementation.** A game that plays sound submits exactly one
task, an AX or Zelda microcode image, and then leaves it running for the whole session. Everything
after the initial handshake is that one task being handed work and reporting back. So an
implementation may:

* answer the boot handshake (§A.4),
* announce the task as started (`0xDCD10000`),
* accept whatever the game sends and *ignore its content*,
* report `0xDCD10002` after a plausible delay, and
* accept the acknowledgement.

That loop is sufficient for the SDK's driver to keep calling the game's audio callbacks, which is
what the game's frame pacing depends on. It produces no sound. Parts B–D are how to produce sound.

## A.2 The mailbox registers

Four 16-bit registers form two 32-bit mailboxes, at the base of the DSP interface block.

| Offset | libogc index | Direction | Half |
|---|---|---|---|
| `0xCC005000` | `_dspReg[0]` | CPU → DSP | high (bits 31:16) |
| `0xCC005002` | `_dspReg[1]` | CPU → DSP | low (bits 15:0) |
| `0xCC005004` | `_dspReg[2]` | DSP → CPU | high (bits 31:16) |
| `0xCC005006` | `_dspReg[3]` | DSP → CPU | low (bits 15:0) |

### The "message waiting" bit

**Bit 31 of each mailbox — that is, bit 15 of its high half-word — means "a message is sitting here
that the other side has not taken yet."** libogc tests exactly that bit in both directions
(`dsp.c:368-386`):

```c
u32 DSP_CheckMailTo(void)      { return _SHIFTR(_dspReg[0],15,1); }   // 1 = not yet taken
u32 DSP_CheckMailFrom(void)    { return _SHIFTR(_dspReg[2],15,1); }   // 1 = mail is waiting
```

Who sets it and who clears it differs by direction, and this asymmetry is the part implementations
get wrong:

* **CPU → DSP.** The *hardware* sets bit 31 when the CPU writes the **low** half. The *DSP* clears
  it when it takes the message. libogc therefore spins after every single send — `DSP_SendMailTo(x);
  while(DSP_CheckMailTo());` appears twelve times in `__dsp_boottask` alone (`dsp.c:140-159`) — and
  that spin is the whole flow control of the protocol. An implementation that never sets the bit
  (because it consumes the mail instantly) makes every one of those spins a no-op, which is correct
  and fast. An implementation that sets it and forgets to clear it hangs the game.
* **DSP → CPU.** The bit is part of the *value the microcode sends*. Every mail in this document —
  `0x8071FEED`, `0x80544348`, `0xDCD1000x` — has bit 31 set, and that is not decoration: it is the
  flag. The **CPU clears it by reading the low half**.

That last point fixes the read order. The CPU must read high then low:

```c
// dsp.c:388-396
u32 DSP_ReadMailFrom(void)
{
    return (_SHIFTL(_dspReg[2],16,16)|(_dspReg[3]&0xffff));
}
```

C's evaluation order is unspecified here, but the hardware is not: **reading the high half is
non-destructive and reading the low half consumes the message and clears bit 31.** Dolphin's HLE
mail handler implements precisely that — a read of the high half latches the pending mail without
dequeuing it; a read of the low half dequeues it and masks bit 31 out of the latched value, so a
*subsequent* read of the high half shows the previous message's low fifteen bits with the flag
clear. `gc_io.c:140-150` already models this: the high read ORs in `0x8000` from a `dsp_mail_waiting`
flag, the low read clears the flag.

Writing is the mirror: high first, low second, and **the write of the low half is what commits the
message.**

```c
// dsp.c:398-405
void DSP_SendMailTo(u32 mail)
{
    _dspReg[0] = _SHIFTR(mail,16,16);
    _dspReg[1] = (mail&0xffff);
}
```

`gc_io.c:995` hooks only the low write, reassembling the 32-bit value from the stored high half.
That is the correct place for the hook.

`DSP_ReadCPUtoDSP()` (`dsp.c:407-415`) reads the CPU→DSP mailbox back, so the registers are
readable in both directions; only bit 31's meaning changes. `gc_io.c:263-266` masks bit 15 off that
read-back unconditionally, which is the "message is always taken instantly" model.

## A.3 The DSP interrupt

The control register is at **`0xCC00500A`**, `_dspReg[5]` in libogc. It carries **three independent
interrupts**, each as a *status* bit with its *mask* in the bit immediately above it
(`dsp.c:44-54`, and the same block repeated verbatim in `audio.c:42-53`):

| Bit | Value | Name | Meaning |
|---|---|---|---|
| 0 | `0x0001` | `DSPCR_RES` | Reset the DSP. Self-clearing. |
| 1 | `0x0002` | `DSPCR_PIINT` | The CPU asserts an interrupt *to* the DSP |
| 2 | `0x0004` | `DSPCR_HALT` | Halt the DSP |
| 3 | `0x0008` | `DSPCR_AIINT` | **Status:** the audio DMA (AID) finished a buffer. Write 1 to clear. |
| 4 | `0x0010` | `DSPCR_AIINTMSK` | Mask for bit 3 |
| 5 | `0x0020` | `DSPCR_ARINT` | **Status:** an ARAM DMA finished. Write 1 to clear. |
| 6 | `0x0040` | `DSPCR_ARINTMSK` | Mask for bit 5 |
| 7 | `0x0080` | `DSPCR_DSPINT` | **Status: the DSP interrupted the CPU.** Write 1 to clear. |
| 8 | `0x0100` | `DSPCR_DSPINTMSK` | Mask for bit 7 |
| 9 | `0x0200` | `DSPCR_DSPDMA` | An ARAM DMA is in progress (read-only status) |
| 10 | `0x0400` | — | The init microcode is running; see §A.4 |
| 11 | `0x0800` | `DSPCR_DSPRESET` in libogc | The init bit; see §A.4 |

**The DSP interrupt is bit 7, its mask is bit 8.** That is the one the task protocol runs on.

### How it reaches the processor

The three status/mask pairs are OR'd into **one** line, which is **processor-interface interrupt
cause bit 6** (`gc.h:53`, `GC_INT_DSP = 6`; Dolphin names the same bit `INT_CAUSE_DSP = 0x40`).
The condition is `((status >> 1) & status & 0x00A8) != 0` — each status bit AND its neighbour above
— which `gc_io.c:224-230` already implements:

```c
const int asserted = ((c & 0x0008u) && (c & 0x0010u)) || ((c & 0x0020u) && (c & 0x0040u)) ||
                     ((c & 0x0080u) && (c & 0x0100u));
gc_set_interrupt(m, GC_INT_DSP, asserted);
```

libogc's three handlers all sit on that one PI bit and are distinguished purely by which status bit
is set: `IRQ_DSP_AI`, `IRQ_DSP_ARAM`, `IRQ_DSP_DSP` (`tuxedo/interrupt.h:69-82`). Each handler
acknowledges *only its own* bit, and the idiom is uniform — write back the register with your own
status bit set and the other two clear, so that writing 1 clears yours and writing 0 leaves theirs
alone:

```c
// dsp.c:319-323, the DSP-mail handler
static void __dsp_inthandler(u32 nIrq,void *pCtx)
{
    _dspReg[5] = (_dspReg[5]&~(DSPCR_AIINT|DSPCR_ARINT))|DSPCR_DSPINT;
    if(__dsp_intcb) __dsp_intcb();
}
```

```c
// audio.c:126-128, the audio-DMA handler
_dspReg[5] = (_dspReg[5]&~(DSPCR_DSPINT|DSPCR_ARINT))|DSPCR_AIINT;
```

Two things an implementation must get right, both already noted in `gc_io.c`:

* **The line is a level, not a pulse.** If the status bit stays set after the handler runs, the
  machine re-enters the handler forever. `gc_io.c:220-223` says this in as many words.
* **The three status bits are write-1-to-clear, and a write of 0 to one of them must leave it
  alone** — otherwise the handler idiom above, which writes all three back in one store, would clear
  the other two devices' interrupts as a side effect (`gc_io.c:272-274`).

### Mail does not interrupt by itself

This is the trap. **Writing a mail to the from-DSP mailbox does not raise bit 7.** On hardware the
microcode raises the interrupt explicitly, as a separate act, after writing the mail. Dolphin's HLE
makes this an argument to its mail-push call — pushing a mail with the interrupt flag set generates
`INT_DSP`, pushing one without it does not — and the AX microcode always pushes with the flag set.

For a high-level implementation the practical rule is: **whenever you place a `0xDCD1000x` mail,
set bit 7 and re-evaluate the line.** The one subtlety Dolphin models is queueing: if a mail is
already pending when a second is pushed, the interrupt for the second is deferred until the CPU
reads the first out. An implementation that never queues more than one mail at a time does not need
that.

The opposite direction, bit 1 (`DSPCR_PIINT`), is the CPU interrupting the DSP. `DSP_AssertInt()`
(`dsp.c:417-426`) is the only user, and the only caller is `DSP_AssertTask()` (`dsp.c:488-511`),
which uses it to force a running task to yield so a higher-priority one can pre-empt it. A
high-level implementation that never multiplexes tasks can treat a write of bit 1 as "the game
wants the current task to yield soon" and answer with `0xDCD10002`.

## A.4 Boot: the ROM handshake

Out of reset the DSP runs its mask ROM, and the ROM announces itself:

> **`0x8071FEED`** — sent by the **ROM**, unprompted, as soon as it starts.

`__dsp_boottask()` waits for it before doing anything else (`dsp.c:131-132`), and only checks its
value in debug builds (`dsp.c:134`).

The ROM then speaks a small upload protocol. Mails come in **pairs**: a selector of the form
`0x80F3xxxx`, then the value. Anything the ROM does not recognise as a selector it echoes back as
`0xFEEE0000 | (mail & 0xFFFF)`.

| Selector | The word after it means | Sent by libogc as |
|---|---|---|
| `0x80F3A001` | Main-memory address of the microcode image | `task->iram_maddr` (`dsp.c:140-143`) |
| `0x80F3C002` | Destination address in DSP instruction memory | `task->iram_addr & 0xFFFF` (`dsp.c:144-147`) |
| `0x80F3A002` | Length of the image in bytes | `task->iram_len` (`dsp.c:148-151`) |
| `0x80F3B002` | Length of the data image | `0` (`dsp.c:152-155`) |
| `0x80F3D001` | **Entry vector — and the trigger.** On receiving this the ROM copies the image and jumps to it | `task->init_vec` (`dsp.c:156-159`) |

So the boot sequence is exactly ten mails, in five pairs, in that order, with a spin on
`DSP_CheckMailTo()` between every one.

`gc_io.c:118-138` implements this with two of the five selectors honoured and the rest falling
through a `default: break;` — correct, because only `0x80F3A001` (where) and `0x80F3D001` (go) are
load-bearing if you are not really copying anything.

**The microcode announces itself next.** The value depends on which microcode was loaded. The one
the SDK loads during start-up is the *init* microcode, and it sends:

> **`0x80544348`** — sent by the **init microcode**, once, on entry.

`gc_io.c:108` names this `GC_DSP_INIT_HELLO` and sends it from `dsp_boot_ucode()`. Both values are
corroborated by Dolphin's HLE, whose ROM stub sends `0x8071FEED` on initialisation and whose init
stub sends `0x80544348` and then ignores all mail.

### The other way in: the init bit

There is a second path to the init microcode that does not involve the mailbox at all, and it is
the one the SDK's `DSPInit()` uses. **Bit 11 (`0x0800`) of the control register is an "init"
bit, and the init microcode boots on its *falling* edge.** While the init microcode is starting,
**bit 10 (`0x0400`) reads as 1**, and it clears a short time later — Dolphin measured roughly
**130 timebase ticks** on real hardware and notes the figure is not perfectly consistent.
`gc_io.c:284-288` implements exactly this, with the same 130-tick figure.

Two naming hazards worth stating plainly, because they will mislead anyone reading libogc:

* **libogc calls bit 11 `DSPCR_DSPRESET` and comments it "Reset DSP".** It is not the reset. The
  reset is bit 0, which libogc calls `DSPCR_RES` and comments "reset DSP" in lower case.
  `DSP_Reset()` (`dsp.c:428-437`) writes **both**.
* **libogc's own `DSP_Init()` does not produce the falling edge.** It sets bit 11 and then writes
  the register back with only `HALT` and the three status bits cleared (`dsp.c:338-339`) — bit 11
  reads back set and stays set. So a libogc program never boots the init microcode this way; a
  Nintendo SDK program does. An implementation must handle both the mailbox path and the falling
  edge, because the hwtests programs in this repository are SDK programs.

## A.5 The five mails from the DSP

These are the values in question, and **all five are sent by the microcode, not by the ROM.** The
ROM's entire vocabulary is `0x8071FEED` and the `0xFEEExxxx` echo. Any `0xDCD1xxxx` on the wire
means a microcode is running.

The CPU dispatches on them in `__dsp_def_taskcb()` (`dsp.c:212-317`), which is the default DSP
interrupt callback installed by `DSP_Init()` (`dsp.c:333`). It reads one mail, spinning until one is
there, and switches:

### `0xDCD10000` — the task has initialised

The microcode has finished its cold start and is running.

* **CPU action:** set `state = DSPTASK_RUN`; call `init_cb` (`dsp.c:229-232`).
* **Acknowledgement: none.** The driver sends nothing back.
* **What the callback does, for AX:** submits the first command list. This matters — §B.1.

### `0xDCD10001` — the task has resumed

The microcode has re-entered at `resume_vec` after a yield, or after being swapped back in.

* **CPU action:** set `state = DSPTASK_RUN`; call `res_cb` (`dsp.c:233-236`).
* **Acknowledgement: none.**

### `0xDCD10002` — the task is offering to yield

The microcode has reached a point where it can be swapped out and is asking what to do. **This is
the only mail in the ordinary steady state of a running audio task**, and the only one whose reply
is a genuine decision.

* **Acknowledgement: required.** One of two values (`dsp.c:237-278`):

| Reply | When | What follows |
|---|---|---|
| **`0xCDD10003`** | There is nothing else to run — the task offering to yield is the only one, or it is the one that was just asserted | Nothing. The driver then calls `res_cb` itself, because the microcode simply carries on |
| **`0xCDD10001`** | Another task should run now | The **ten-word context-switch block** of §A.6, immediately, with the usual spin between words |

* If the task carries the `DSPTASK_CANCEL` flag, the driver rewrites the mail to `0xDCD10003`
  before dispatching (`dsp.c:224-226`) — a cancelled task's yield is treated as a completion.

Note the shape of the common case: **reply `0xCDD10003`, then call `res_cb`.** For an audio game
with one task, that is the entire per-frame loop, and `res_cb` is where the game submits the next
command list.

### `0xDCD10003` — the task is done

The microcode has run to completion and will not resume.

* **CPU action:** call `done_cb`, then reply, then unlink the task (`dsp.c:279-311`).
* **Acknowledgement: required.** Again two values:

| Reply | When | What follows |
|---|---|---|
| **`0xCDD10002`** | This was the only task; the list becomes empty | Nothing. The DSP returns to its ROM |
| **`0xCDD10001`** | Another task is waiting | The ten-word block of §A.6, with the *outgoing* task's three data-save words zeroed, because a finished task's state need not be preserved (`__dsp_exectask(NULL, ...)`, `dsp.c:285` and `dsp.c:307`) |

**AX never sends this.** An AX task runs for the life of the program. `0xDCD10003` is what the
short-lived microcodes send — the memory-card microcode and the GBA-link microcode both announce
`0xDCD10000` when they start and `0xDCD10003` when their one job is finished. An implementation
that only ever runs an audio microcode will never need to send it, but must still be able to
*receive* the reply, because a game that touches a memory card will boot the card microcode as a
second task.

### `0xDCD10004` — the task is asking for something

* **CPU action:** call `req_cb` (`dsp.c:312-314`).
* **Acknowledgement: none** from the driver. Whatever the request needs, `req_cb` supplies, and it
  is free to send mail of its own.

**Unverified:** what AX uses this for, or whether it uses it at all. Dolphin's AX HLE never sends
it, and its header describes it only as "triggers a callback, no response is sent". The libogc
dispatch is unambiguous, so an implementation knows what to *do* with it; what is not established
is what would make a real AX microcode send one. A DSP-LLE trace of a game that registers a
non-null `req_cb` would settle it.

### Summary table

| Mail | Sent by | Meaning | CPU does | Reply to `0xCC005000` |
|---|---|---|---|---|
| `0x8071FEED` | **ROM** | I am awake | Begins the upload protocol | the `0x80F3xxxx` pairs |
| `0x80544348` | init **microcode** | I am running | — | — |
| `0xDCD10000` | microcode | initialised | `init_cb` | none |
| `0xDCD10001` | microcode | resumed | `res_cb` | none |
| `0xDCD10002` | microcode | offering to yield | decides | `0xCDD10003` stay, or `0xCDD10001` + 10 words |
| `0xDCD10003` | microcode | finished | `done_cb`, unlink | `0xCDD10002` idle, or `0xCDD10001` + 10 words |
| `0xDCD10004` | microcode | request | `req_cb` | none |

## A.6 The ten-word context-switch block

Both `0xCDD10001` replies are followed by ten mails with no interleaved response, sent by
`__dsp_exectask(exec, hire)` (`dsp.c:162-210`). `exec` is the task being swapped **out** (or `NULL`
if it has finished), `hire` the one being swapped **in**.

| # | Value | Meaning |
|---|---|---|
| 0 | `exec->dram_maddr`, or 0 | Main-memory address to **save** the outgoing task's data memory to |
| 1 | `exec->dram_len`, or 0 | How much |
| 2 | `exec->dram_addr`, or 0 | From where in DSP data memory |
| 3 | `hire->iram_maddr` | Main-memory address of the incoming microcode image |
| 4 | `hire->iram_len` | Its length |
| 5 | `hire->iram_addr` | Where in DSP instruction memory to put it |
| 6 | `hire->init_vec` **or** `hire->resume_vec` | Entry point — `init_vec` if the incoming task's state is `DSPTASK_INIT`, `resume_vec` otherwise |
| 7 | `hire->dram_maddr`, or 0 | Main-memory address of the incoming data image |
| 8 | `hire->dram_len`, or 0 | Its length |
| 9 | `hire->dram_addr`, or 0 | Where in DSP data memory |

When the incoming task is cold (`state == DSPTASK_INIT`), words 7–9 are sent as zeros
(`dsp.c:189-198`) — there is no saved state to restore.

Dolphin's HLE counts the same ten words in the same order and switches microcode when the tenth
arrives, which is independent corroboration of the ordering.

**For a high-level implementation:** the ten words tell you *which* microcode is coming. If you
identify microcodes by hashing the image at word 3 for word 4 bytes, this is where you do it. If
you identify them more crudely — "a task with a large image that the game keeps alive is the audio
one" — you can discard all ten and still answer the protocol, at the cost of not knowing whether
the game is running AX or Zelda.

## A.7 What the sequence actually looks like, for AX

Putting Part A together with §B.1, the whole life of an audio task:

```
DSP → CPU   0x8071FEED                     ROM awake
CPU → DSP   0x80F3A001, <image address>    ] five selector/value pairs,
CPU → DSP   0x80F3C002, <iram address>     ] spinning on bit 31 of the
CPU → DSP   0x80F3A002, <length>           ] to-DSP mailbox after each
CPU → DSP   0x80F3B002, 0                  ]
CPU → DSP   0x80F3D001, <init vector>      ] the last one boots it
DSP → CPU   0xDCD10000                     + DSP interrupt.  init_cb runs
CPU → DSP   0xBABE0000 | <halfword count>  the first command list
CPU → DSP   <command list address>
   ... the microcode executes the list ...
DSP → CPU   0xDCD10002                     + DSP interrupt.  after ~2500 cycles
CPU → DSP   0xCDD10003                     "carry on".  res_cb runs
CPU → DSP   0xBABE0000 | <halfword count>  the next command list
CPU → DSP   <command list address>
   ... forever ...
```

**The delay between the command list and `0xDCD10002` is load-bearing.** Dolphin's AX HLE
deliberately defers that mail's interrupt, and its comment records why: at least one commercial
title hangs at boot if the interrupt arrives too soon after the command list is submitted, because
it tracks a "command list cycles" variable of its own. The figures Dolphin cites, measured against
DSP-LLE, are ~160 000 CPU cycles for that title, ~50 000 for another, ~718 000 for a third, with
814 as the minimum that avoids the hang and ~2500 as a safe floor derived from what the game itself
initialises the variable to for an empty list.

**Take the practical lesson, not the numbers:** do not answer a command list synchronously.
Schedule the yield mail at least a few thousand CPU cycles out. An implementation that replies in
zero cycles has a real chance of hanging a game in a way that looks nothing like an audio bug.

---

# Part B — the AX command list

Everything from here down is what the *AX microcode* does. It is not needed to satisfy Part A.

The numbering, argument counts and semantics in this part come from Dolphin's AX HLE, read as a
behaviour reference. It is a second implementation, not hardware, and there is no equivalent of
`gxtest` for the DSP in this repository. Where a claim is only as good as Dolphin's guess, it is
marked.

## B.1 How the CPU hands over a list

Two mails, in order, with the usual spin between:

1. **`0xBABE0000 | n`**, where **`n` is the length of the list in 16-bit halfwords** (Dolphin
   masks the low sixteen bits and treats them as a halfword count, then copies `n * 2` bytes).
2. **The physical main-memory address of the list.**

The microcode DMAs the list into its data memory and executes it. Dolphin's buffer is 512
halfwords and it refuses anything that size or larger, which is a reasonable bound on what a real
list can be. The list must be in main memory and must have been flushed out of the CPU's cache
before the second mail is sent.

Note the state machine on the DSP side: after a list has been executed and `0xDCD10002` answered,
the microcode expects the *next* mail to be a `0xBABE` header again. The acknowledgement
`0xCDD10003` is not itself followed by anything from the DSP — the CPU simply proceeds to the next
header. Dolphin notes that the real microcode does not actually check the `0xCDD1` prefix on the
acknowledgement.

## B.2 The shape of a command

A command list is **an array of big-endian 16-bit halfwords**. There is no length field, no
alignment padding and no per-command header beyond the opcode.

* The first halfword is an **opcode**.
* It is followed by a **fixed number of argument halfwords determined by the opcode** — there is no
  encoded length, so a walker must know the table in §B.3 or it will desynchronise.
* 32-bit values (always addresses, in practice) are **two halfwords, high first**.
* The list ends at the `END` opcode, or when the walker runs off the end of what it copied.

A walker is therefore: `idx = 0; loop { cmd = list[idx++]; consume arguments; }` until `END`.
Dolphin also terminates on an unknown opcode, which is the only safe thing to do, since the
argument count is unknown.

## B.3 The command table

Opcodes are small integers, dense from 0x00 to 0x13.

| Op | Name | Args (halfwords) | What it does |
|---|---|---|---|
| `0x00` | **SETUP** | 2 (address) | Initialise the nine mixing buffers from a table in memory — §B.4 |
| `0x01` | DOWNLOAD_AND_MIX | 5 (address, 3 volumes) | Read nine buffers of samples from memory, scale each group by one of three volumes, add into main / auxA / auxB |
| `0x02` | **SET_VOICE_LIST** | 2 (address) | Record the address of the head of the parameter-block list. Does not process anything — §B.4 |
| `0x03` | **PROCESS** | 0 | Walk the voice list and mix every voice — §B.4 |
| `0x04` | MIX_AUXA | 4 (write address, read address) | Upload the three auxA buffers to the first address; add three buffers read from the second into main L/R/S |
| `0x05` | MIX_AUXB | 4 | The same for auxB |
| `0x06` | UPLOAD_LRS | 2 (address) | Write main L, R and S to memory |
| `0x07` | SET_MAIN_LR | 2 (address) | *Replace* main L and R with one buffer read from memory; zero main S |
| `0x08` | — | **10** | **Unverified:** unimplemented by Dolphin, which skips ten argument halfwords. The count is the only fact here |
| `0x09` | MIX_AUXB_NOWRITE | 2 (read address) | As `0x05` but with no upload |
| `0x0A`–`0x0C` | — | 0 | No-ops in every GameCube microcode revision Dolphin handles |
| `0x0D` | **CHAIN** | 3 (address, halfword count) | Fetch a *new* command list from memory and restart the walk at its index 0 — §B.4 |
| `0x0E` | **OUTPUT** | 4 (surround address, L/R address) | Write the finished mix out — §D.2 |
| `0x0F` | **END** | 0 | Stop walking |
| `0x10` | MIX_AUXB_LR | 4 (upload address, download address) | Upload auxB L/R; then read two buffers back, replacing auxB L/R and adding them into main L/R |
| `0x11` | SET_OPPOSITE_LR | 2 (address) | Replace main L with the negation of a buffer and main R with the buffer; zero main S |
| `0x12` | COMPRESSOR | 4 (threshold, release frames, table address) | A look-ahead-free limiter over main L/R, driven by a table of per-sample gain ramps |
| `0x13` | SEND_AUX_AND_MIX | 12 (six addresses) | Upload auxA L/R/S and auxB S; then download four buffers and add them into main L, main R, auxB L, auxB R |

One revision caveat that matters if you hash the microcode: the oldest GameCube AX revision Dolphin
knows (`crc 0x4e8a8b21`, used by a long list of 2001–2002 titles) **does not implement `0x12`**, and
does not range-check the opcode, so a list containing it on that revision jumps into the middle of a
mixer routine. Do not emulate that; it is a crash, not a feature.

## B.4 The four that matter

An implementation that handles `SETUP`, `SET_VOICE_LIST`, `PROCESS`, `OUTPUT` and `END`, and that
skips every other opcode's arguments correctly, produces the main stereo mix. The rest are
auxiliary sends and effects returns that the CPU side of the SDK uses for reverb.

**`0x00` SETUP — initialise the mixing buffers.** The argument is the address of a table of **nine
triples of halfwords**, one per mixing buffer, in the order main L, main R, main S, auxA L, auxA R,
auxA S, auxB L, auxB R, auxB S. Each triple is `(value_hi, value_lo, delta)`: a signed 32-bit
starting value and a signed 16-bit per-sample increment. If the 32-bit value is zero the buffer is
zeroed; otherwise buffer sample *j* is initialised to `value + j * delta`. So a frame does not
necessarily start from silence — the CPU can prime the mix with a ramp.

The mixing buffers are **32-bit** and hold **5 ms at 32 kHz = 160 samples** each.

**`0x02` SET_VOICE_LIST — point at the voices.** The two halfwords are a 32-bit main-memory address.
The command records it and does nothing else. A list will typically issue this once and then
`PROCESS`.

**`0x03` PROCESS — mix every voice.** Starting from the recorded address, walk a **singly linked
list of parameter blocks**: read a block, mix it, write it back (voices carry mutable state — see
Part C), and follow its `next` pointer. **The list ends at a null `next` pointer**, i.e. when the
32-bit next-address is zero. A parameter block is read *once* per `PROCESS`, and the five
milliseconds are then mixed as **five separate 1 ms sub-frames of 32 samples each**, with the
per-millisecond update list (§C.6) applied before each.

**`0x0E` OUTPUT — write the mix out.** Four argument halfwords: the **first** pair is the address
of the surround buffer, the **second** pair is the address of the stereo output buffer. See §D.2
for the formats; they are not the same.

**`0x0D` CHAIN** is worth knowing about even though it is rare: it replaces the current list with a
new one and restarts the walk. A naive walker that treats it as "skip three halfwords" will read
the rest of the old list as garbage.

**`0x0F` END** terminates. Every well-formed list has one.

---

# Part C — a voice

## C.1 The parameter block

Each voice is a **parameter block** in main memory (or, for some titles, in ARAM), read and written
back by every `PROCESS`. Its layout is an ordered sequence of 16-bit fields, and **the order is
load-bearing**: the per-millisecond update mechanism of §C.6 addresses fields by their halfword
index into the block, so games depend on the exact offsets.

In order, for the GameCube AX parameter block:

| Group | Fields | Notes |
|---|---|---|
| Links | `next_pb` (hi, lo), `this_pb` (hi, lo) | 32-bit main-memory addresses. `next_pb == 0` ends the voice list |
| Resampler select | `src_type`, `coef_select` | §C.5 |
| Mix routing | `mixer_control` | §C.5 |
| State | `running`, `is_stream` | **`running == 1` means play; any other value means skip this voice entirely.** `is_stream == 1` changes loop behaviour — §C.4 |
| Mixer sends | nine `(volume, volume_delta)` pairs | Order: main L, main R, auxA L, auxA R, auxB L, auxB R, **auxB S, main S, auxA S**. The last three are deliberately out of pattern — Dolphin's comment attributes the ordering to letting the microcode reuse a two-channel mix routine for adjacent pairs |
| Initial time delay | `on`, address (hi, lo), two offsets, two targets | A per-channel delay line for 3D positioning. **Unverified:** Dolphin does not implement it and only counts how often games enable it |
| Updates | five per-millisecond counts, then an address (hi, lo) | §C.6 |
| De-pop | nine `s16`, one per mixer send | The last sample the voice contributed to each buffer, so that a voice which stops can be ramped down instead of clicking. **Unverified:** Dolphin stores these but does not act on them |
| Volume envelope | `cur_volume`, `cur_volume_delta` | §C.5 |
| (reserved) | three halfwords | |
| Audio address | `looping`, `sample_format`, `loop_addr` (hi, lo), `end_addr` (hi, lo), `cur_addr` (hi, lo) | §C.2, §C.4 |
| ADPCM | sixteen `s16` coefficients, `gain`, `pred_scale`, `yn1`, `yn2` | §C.3 |
| Resampler | `ratio` (hi, lo), `cur_addr_frac`, four `s16` history samples | §C.5 |
| ADPCM loop | `pred_scale`, `yn1`, `yn2` | §C.4 |
| Low-pass filter | `on`, `yn1`, `a0`, `b0` | §C.5 |
| | `loop_counter` | Incremented each time a *streamed* voice wraps — §C.4 |

**Unverified:** the total size of the block and how much trailing padding it carries. Dolphin's
structure ends with 24 halfwords of padding after `loop_counter`, which gives 244 bytes, and that is
not an obviously natural size. What *is* established is that **there are at least two GameCube
layouts**: the oldest revision (`crc 0x4e8a8b21`) has **no low-pass filter block at all**, so
`loop_counter` sits four halfwords earlier and the whole block is four halfwords shorter. Dolphin
reads and writes that revision's blocks with the filter fields skipped. An implementation that
identifies microcodes by hash must carry both layouts; one that does not must pick the modern one
and accept that a handful of 2001–2002 titles will have their `loop_counter` misplaced.

## C.2 Sample formats, and what an address means

The `sample_format` field is a small bit-packed value. Its layout, from Dolphin's model of the DSP's
sample accelerator:

| Bits | Field | Values |
|---|---|---|
| 1:0 | **Access size** | 0 = 4-bit, 1 = 8-bit, 2 = 16-bit, 3 = invalid |
| 3:2 | **Decoder** | 0 = ADPCM, 1 = PCM from a register with no address advance, 2 = PCM from memory, 3 = PCM from a register with address advance |
| 5:4 | **Gain divisor** | 0 = ÷2048, 1 = ÷1, 2 = ÷65536, 3 = invalid |
| 15:6 | — | **Unverified:** Dolphin warns if any are set and otherwise ignores them |

The three values AX actually uses:

| Constant | Value | Decodes to |
|---|---|---|
| `ADPCM` | `0x0000` | 4-bit access, ADPCM decoder, gain ÷2048 |
| `PCM16` | `0x000A` | 16-bit access, PCM decoder, gain ÷2048 |
| `PCM8` | `0x0019` | 8-bit access, PCM decoder, gain ÷1 |

**The critical consequence: `cur_addr`, `loop_addr` and `end_addr` are not byte addresses. They are
in units of the access size.** For 4-bit ADPCM they count **nibbles**; for PCM8 they count bytes;
for PCM16 they count 16-bit samples. The byte address is recovered as `addr >> 1`, `addr` and
`addr * 2` respectively. An implementation that treats them uniformly as byte addresses will read
ADPCM data at twice the rate and PCM16 at half.

The addresses are masked before use: start and end to 30 bits, the current address to 30 bits plus
bit 31 (bit 31 selects writing rather than reading, which AX voices never do).

`end_addr` is **inclusive** — it is the address of the last sample, not one past it.

## C.3 ADPCM decoding

Yes, this can be established, and the important finding is negative:

> **There is no fixed ADPCM coefficient table on the GameCube.** The sixteen coefficients — eight
> pairs — are **per-voice data, carried in the parameter block**, and they come from the sound file
> the game shipped. A decoder cannot be written against a constant table the way a GBA or NDS ADPCM
> decoder can.

This is a structural fact about the format rather than a measurement: the parameter block carries
`s16 coefs[16]`, the tool chain's `.dsp` file header carries the same sixteen values, and they were
fitted per sound at encode time. The decoder selects a *pair* out of the sixteen with an index that
changes every frame.

### The frame layout

ADPCM data is a sequence of **8-byte frames**: one **predictor/scale byte**, then seven bytes
holding **fourteen 4-bit samples**, high nibble first.

In the nibble-address arithmetic the accelerator uses, a frame is sixteen nibble-addresses: when
the current address becomes a multiple of 16, the byte at `(addr & ~15) >> 1` is read as the new
predictor/scale and the address is advanced by 2, skipping the header's two nibbles. Fourteen
sample nibbles then follow before the next multiple of 16.

### The header byte

| Bits | Meaning |
|---|---|
| 6:4 | **Coefficient index**, 0…7 — selects the pair `coefs[2i]`, `coefs[2i+1]` |
| 3:0 | **Scale exponent** — the multiplier is `1 << exponent` |
| 7 | Masked off; the accelerator keeps only seven bits of the value |

### The step

For each 4-bit sample `n`, sign-extended from 4 bits (so `n >= 8` becomes `n - 16`):

```
scale  = 1 << (pred_scale & 0xF)
i      = (pred_scale >> 4) & 7
out    = clamp_s16( scale * n  +  ((0x400 + coefs[2i] * yn1 + coefs[2i+1] * yn2) >> 11) )
yn2    = yn1
yn1    = out
```

Three details that decide whether the output matches:

* **The `0x400` is a rounding constant**, half of the 2048 the shift divides by. Omitting it biases
  every sample downward.
* **The shift is 11, arithmetic**, and it applies to the predictor term only — the scaled residual
  is added after.
* **The result is clamped, not truncated**, to the signed 16-bit range, and it is the *clamped*
  value that becomes the next `yn1`.

`yn1` and `yn2` are the two previous output samples and live in the parameter block, so decoding
resumes exactly where it left off between frames.

### PCM

The same datapath runs for PCM, which is why `yn1`/`yn2`/`gain` exist for a format that does not
appear to need them:

```
out = ((gain * raw) >> s)  +  ((coefs[2i] * yn1) >> s)  +  ((coefs[2i+1] * yn2) >> s)
```

where `s` is 11, 0 or 16 from the gain-divisor field. With the coefficients zeroed this is a plain
gain stage: for `PCM16` (`s = 11`) a `gain` of `0x800` is unity; for `PCM8` (`s = 0`) a `gain` of
`0x100` widens a byte to a halfword. Note the **result is not clamped** in the PCM path, unlike
ADPCM — it is truncated to 16 bits.

**Unverified:** whether real AX voices ever ship non-zero coefficients with a PCM format. The
datapath allows it; nothing read here says a game does it.

## C.4 Looping and the end of a sample

The accelerator compares the current address against `end_addr` after each sample and, when it
matches, resets the current address to `loop_addr` (which the microcode has loaded as the
accelerator's "start" address) and raises an end-of-sample condition. What happens next depends on
two fields:

* **`looping == 0`** — the voice stops: **`running` is set to 0** and the voice is skipped on every
  subsequent `PROCESS` until the CPU restarts it.
* **`looping != 0` and `is_stream != 1`** — a one-shot loop. The ADPCM state is reloaded from the
  **ADPCM loop block**: `pred_scale`, `yn1`, `yn2` are all restored from the three saved values. This
  is what makes a loop seamless: the decoder's history at the loop point is a property of the sound,
  precomputed at encode time, not something that can be carried over from the end of the buffer.
* **`looping != 0` and `is_stream == 1`** — a streamed loop. Only `pred_scale` is restored; `yn1`
  and `yn2` are **kept**, because the CPU is refilling the buffer behind the playhead and the
  waveform is genuinely continuous across the wrap. **`loop_counter` is incremented**, which is how
  the CPU knows a wrap happened and how many buffers it is behind.

There are two documented edge cases in the ADPCM path where the end-of-sample behaviour differs —
when `end_addr` is 16-nibble-aligned the loop target is `start + 1` rather than `start`, and when
`end_addr & 15 == 1` the detection fires one sample early — and in both the predictor/scale register
is *not* updated. **Unverified:** these come from Dolphin's accelerator model, are described there
as hardware behaviour, and are not corroborated by any second source read here. They are unlikely to
matter for AX voices, whose loop points are generated by the tool chain and land on frame
boundaries, but an implementation chasing a one-sample click at a loop point should look here first.

One more piece of state worth modelling: after an end-of-sample condition the accelerator **stops
returning data** until `yn2` is written. That is why the stream path above writes `yn1` and `yn2`
back with their own values — it looks like a no-op but it is what restarts reads.

## C.5 The rest of a voice: resampling, envelope, filter, sends

`PROCESS` handles one voice for one millisecond — 32 samples at 32 kHz — in this order.

**0. Skip if not running.** `running != 1` means the voice contributes nothing.

**1. Fetch and resample.** The `ratio` field is a 32-bit **16.16 fixed-point** ratio of input rate
to output rate: the integer part in `ratio_hi`, the fraction in `ratio_lo`. The documented valid
range is 1/512 to 4.0. `cur_addr_frac` carries the fractional position between calls, and the four
`last_samples` carry the interpolator's history. `src_type` picks the method:

| `src_type` | Method |
|---|---|
| 0 | **Polyphase** — a four-tap filter whose coefficients come from a 0x800-entry table in the DSP's data ROM, indexed by `coef_select * 0x200` and by the top seven bits of the fractional position |
| 1 | **Linear** — two-tap, weighting by the 16-bit fraction |
| 2 | **Nearest** — no conversion; samples are taken straight from the decoder |

The polyphase table is **not in the parameter block and not in main memory** — it is part of the
DSP's own data ROM. Dolphin loads it from a dumped `dsp_coef.bin` and silently degrades polyphase to
linear when it has not got one. **A from-scratch implementation without that ROM dump should do the
same**: treat `src_type == 0` as linear. The audible difference is small; the difference between
resampling and not resampling is enormous.

**2. Volume envelope.** A per-sample linear ramp applied to the whole voice before any send:

```
sample = clamp_s16( (sample * cur_volume) >> 15 )
cur_volume += cur_volume_delta
```

`cur_volume` is **signed** on the GameCube (it is unsigned on the Wii, which is a real behavioural
difference, not a modelling choice). The delta is applied once per sample, so over a 5 ms frame the
volume moves 160 steps.

**3. Low-pass filter,** if `lpf.on` is non-zero. One-pole, one history value:

```
yn1 = sample = clamp_s16( (a0 * sample + b0 * yn1) >> 15 )
```

**4. The mixer sends.** Nine of them, each a `(volume, volume_delta)` pair, each accumulating into
one of the nine 32-bit mixing buffers:

```
for each sample:
    s = clamp_s16( (sample * volume) >> 15 )
    buffer[i] += s
    volume += volume_delta          # only when this send's ramp bit is set
    dpop_for_this_send = s          # remembers the last contribution
```

Note that the **per-send accumulation is clamped to 16 bits before being added** to a 32-bit
accumulator. The headroom in the buffers is for summing many voices, not for a single loud one.

**Which sends run** is decided by `mixer_control`, and this is the field whose meaning changed
between microcode revisions. For the modern GameCube revisions:

| Bit | Enables |
|---|---|
| 0 | main L |
| 1 | main R |
| 2 | main S |
| 3 | volume ramping on all three main sends |
| 4 | auxA L |
| 5 | auxA R |
| 6 | volume ramping on auxA L and R |
| 7 | auxA S |
| 8 | volume ramping on auxA S |
| 9 | auxB L |
| 10 | auxB R |
| 11 | volume ramping on auxB L and R |
| 12 | auxB S |
| 13 | volume ramping on auxB S |
| 14 | **Unverified:** Dolphin believes it selects the surround input for the auxB sends under Dolby Pro Logic II and does not implement it |

The oldest revision (`crc 0x4e8a8b21`) uses a completely different and much smaller encoding: bit 0
enables both auxA L and R, bit 1 both auxB L and R, bit 2 the surround sends, bit 3 ramping on
everything, and bit 4 switches the whole field into a Pro-Logic interpretation. Main L and R are
always on in that revision. An implementation that applies the modern table to an old microcode will
route almost everything wrongly.

## C.6 Per-millisecond updates

A `PROCESS` reads each parameter block **once** but mixes **five milliseconds**. Games that need
finer timing than 5 ms — the stated motivation is MIDI note timing — use the update mechanism:

* The block carries **five counts**, one per millisecond of the frame, and the address of an update
  array in main memory.
* The array holds up to **32 entries**, each a pair `(halfword offset into the parameter block, new
  value)`.
* Before mixing millisecond *m*, the microcode applies the entries from index `sum of counts[0..m-1]`
  for `counts[m]` entries, writing each new value straight into the block at the given halfword
  offset.

This is why §C.1 insists the field order is load-bearing: the offsets in that array are raw halfword
indices, and a block laid out differently will have arbitrary fields rewritten.

Dolphin bounds-checks both the offset (against the block size) and the count (against the remaining
array). An implementation should do the same; a game that writes a stale count will otherwise walk
off the end.

---

# Part D — where the mixed output goes

Two separate pieces of hardware are involved, in two separate register blocks, with two separate
interrupts. Conflating them is the commonest mistake in this area, and libogc's own naming does not
help: the *audio DMA* lives in the **DSP** block at `0xCC005030` and interrupts through
`DSPCR_AIINT`, while the *audio interface* proper lives at `0xCC006C00` and interrupts through its
own PI cause bit.

## D.1 The audio DMA (AID), at `0xCC005030`

libogc drives it from `audio.c:364-403`, addressing it through the same `_dspReg` array.

| Offset | libogc index | Field |
|---|---|---|
| `0xCC005030` | `_dspReg[24]` | Source address, **bits 28:16**, in the register's bits 12:0 |
| `0xCC005032` | `_dspReg[25]` | Source address, bits 15:0; only bits 15:5 are meaningful (32-byte alignment) |
| `0xCC005034` | `_dspReg[26]` | **Unverified:** never touched by libogc. Dolphin names it a block-length register and marks it "ever used?" |
| `0xCC005036` | `_dspReg[27]` | Bits 14:0 **length in 32-byte blocks**; **bit 15 = enable** |
| `0xCC00503A` | `_dspReg[29]` | Blocks remaining, read-only |

The driving code is short enough to quote in full:

```c
// audio.c:364-403
void AUDIO_InitDMA(u32 startaddr,u32 len)
{
    _dspReg[24] = (_dspReg[24]&~0x1fff)|(_SHIFTR(startaddr,16,13));
    _dspReg[25] = (_dspReg[25]&~0xffe0)|(startaddr&0xffff);
    _dspReg[27] = (_dspReg[27]&~0x7fff)|(_SHIFTR(len,5,15));
}
void AUDIO_StartDMA(void)      { _dspReg[27] = (_dspReg[27]|0x8000); }
void AUDIO_StopDMA(void)       { _dspReg[27] = (_dspReg[27]&~0x8000); }
u32  AUDIO_GetDMABytesLeft(void) { return (_SHIFTL(_dspReg[29],5,15)); }
u32  AUDIO_GetDMALength(void)  { return ((_dspReg[27]&0x7fff)<<5); }
```

So **the unit everywhere is a 32-byte block**: the length is `bytes >> 5`, the remaining count is
`blocks << 5`, and the start address is 32-byte aligned. The `audio.h` documentation says as much —
the address must be 32-byte aligned and the length should be a multiple of 32.

**32 bytes is exactly 8 stereo frames of 16-bit samples.** At 32 kHz that is 250 µs of audio, which
is why Dolphin's comment describes the DMA as servicing itself at 4 kHz.

### The behaviour that matters

**The DMA loops.** When the block count reaches zero the hardware **reloads both the source address
and the block count from the programmed values and raises `DSPCR_AIINT`** — it does not stop. The
CPU's `AIDCallback` (registered by `AUDIO_RegisterDMACallback()`) runs in that interrupt and is
expected to call `AUDIO_InitDMA()` with the *next* buffer before the current one is played out
again. The `audio.h` documentation names the pattern: "internally the DMA buffers are double
buffered."

This is the piece of the audio path with real timing consequences. A game's audio thread is paced by
this interrupt. An implementation that never fires it leaves the game's mixer starved; one that
fires it at the wrong rate makes the game's audio run fast or slow regardless of what the DSP part
does.

The `0xCC00503A` remaining-block count is read by games to find out how far through the buffer the
DMA is. A high-level implementation that consumes buffers instantaneously should still make this
count *decrease plausibly with time*, not sit at zero or at the full count.

## D.2 What the buffer contains

`OUTPUT` (§B.4) writes two different things to two different addresses.

**The stereo output buffer** — the second address pair — is what the AID DMA plays:

* **160 frames** (5 ms × 32 samples at 32 kHz),
* each frame **two 16-bit big-endian samples**,
* each sample the corresponding 32-bit mixing buffer **clamped** to signed 16 bits,
* total **640 bytes**, which is exactly **20 blocks** of the AID's 32-byte unit.

**Channel order within a frame: right first, then left.** That is what Dolphin's HLE writes, and it
is unusual enough to state explicitly. **Unverified against hardware** — it rests on Dolphin alone,
and the only thing that would settle it is a test program that outputs a hard-panned tone and
inspects the DMA buffer, which nothing in this repository does. An implementation that gets it
backwards produces a perfectly plausible-sounding stereo image with the channels swapped, which is
exactly the kind of bug that survives for years.

**The surround buffer** — the first address pair — is different: **160 32-bit values**, unclamped,
written as-is. It is not played by the AID; the CPU picks it up for Dolby Pro Logic encoding or
discards it.

The main L and R mixing buffers are **not** cleared by `OUTPUT`. They are reinitialised by the next
frame's `SETUP`.

## D.3 The audio interface's sample counter, at `0xCC006C00`

A different device, four 32-bit registers (`audio.c:56-67`, with the block based at `0xCC006C00`,
`audio.c:70`):

| Offset | libogc index | Name |
|---|---|---|
| `0xCC006C00` | `_aiReg[0]` | Control |
| `0xCC006C04` | `_aiReg[1]` | Streaming volume — bits 7:0 left, 15:8 right |
| `0xCC006C08` | `_aiReg[2]` | **Sample counter** |
| `0xCC006C0C` | `_aiReg[3]` | Interrupt timing — the count to interrupt at |

Control register bits, from libogc's own constants:

| Bit | Value | Name | Meaning |
|---|---|---|---|
| 0 | `0x01` | `AI_PSTAT` | Streaming play/stop. **The sample counter only advances while this is set** |
| 1 | `0x02` | `AI_AISFR` | Streaming sample rate: 0 = 32 kHz, 1 = 48 kHz. **This is the rate the counter counts at** |
| 2 | `0x04` | `AI_AIINTMSK` | Interrupt mask |
| 3 | `0x08` | `AI_AIINT` | Interrupt status. **Write 1 to clear** |
| 4 | `0x10` | `AI_AIINTVLD` | Interrupt valid |
| 5 | `0x20` | `AI_SCRESET` | Write 1 to reset the sample counter to zero |
| 6 | `0x40` | `AI_DMAFR` | **The AID rate**, and it is *inverted*: `AUDIO_GetDSPSampleRate()` is `(bit6) ^ 1`, so **0 means 48 kHz and 1 means 32 kHz** (`audio.c:420-423`, with libogc's own comment spelling the inversion out) |

This block has **its own interrupt**, on **PI cause bit 5** (`gc.h:52`, `GC_INT_AUDIO = 5`;
Dolphin's `INT_CAUSE_AI = 0x20`) — *not* the DSP's bit 6. libogc registers a separate handler for it
(`IRQ_AI`, `audio.c:273`) which acknowledges by writing bit 3 back.

### How the counter relates to the DMA

**It does not, directly.** The counter is clocked by the *streaming* rate (bit 1), the DMA by the
*DSP* rate (bit 6), and a game may set them differently. The counter counts disc-streaming samples
— the DTK path — and runs whether or not the AID DMA is enabled.

What makes it matter to an implementation with no audio at all is that **games use it as a clock**.
A game writes a target into the interrupt-timing register, starts the counter, and paces work by the
interrupt. `gc_io.c:296-301` records exactly this observation, and names a title that gets through
start-up and then stops at this point.

The semantics `gc_io.c:312-332` implements are right and worth restating:

* The counter advances only while bit 0 is set, at 32 or 48 kHz per bit 1.
* Writing the counter register sets it and restarts the accumulation from that moment.
* Setting bit 5 zeroes it.
* When the count reaches or passes the interrupt-timing value, bit 3 latches; the line is bit 3 AND
  bit 2.
* Bit 3 is write-1-to-clear.

One refinement worth taking from Dolphin's model: the comparison is done as an **unsigned wrapping
window** — the interrupt fires when the target lies within the span the counter just advanced
across — rather than as a plain `count >= target`. That matters when a game sets a target *behind*
the current count, which is a legitimate way of asking for "one full wrap from now". A plain `>=`
fires immediately instead.

**Unverified:** the exact sample rates. 32 000 and 48 000 are what libogc's and Dolphin's names say,
and `__AISRCINIT()` (`audio.c:141-193`) times the counter against the timebase with thresholds of
31 524 ns and 42 024 ns for one sample period, which bracket 1/32 000 (31 250 ns) and are consistent
with the pair — but the real clock is derived from the console's 27 MHz master and is very slightly
off the round numbers. Nothing read here gives the exact divisor. For pacing purposes the round
numbers are within a tenth of a percent and are what the runtime already uses.

---

# Part E — what to build first, in order

Ordered by what blocks what. Each item assumes the ones above it.

**1. The mailbox registers and their "waiting" bits (§A.2).** Nothing else can be tested until mail
moves in both directions with the right half-word order and the right consume-on-low-read
behaviour. Blocks everything.

**2. The DSP interrupt: bit 7 status, bit 8 mask, PI cause bit 6, level-triggered, write-1-to-clear
(§A.3).** Without it the game never enters its DSP callback, so it never sees the mail that is
sitting in the mailbox. This is the single most common way a correct mailbox implementation still
hangs. Blocks 3 onwards.

**3. The boot handshake: `0x8071FEED`, the five `0x80F3xxxx` pairs, `0x80544348`, and the falling
edge of control bit 11 with bit 10 set for ~130 timebase ticks (§A.4).** The SDK's `DSPInit()` runs
before anything else and spins here. Already present in `gc_io.c`. Blocks 4.

**4. ARAM DMA, including the 16 MB wrap (`gc_io.c:173-218`).** The SDK sizes audio memory by writing
past the end and reading address zero back. It is out of order relative to the DSP protocol but
belongs here because it happens during the same start-up and a game that cannot size its audio
memory stops. Already present.

**5. The five `0xDCD1000x` mails and the two acknowledgements (§A.5).** Specifically: send
`0xDCD10000` once after a task boots, accept whatever follows, send `0xDCD10002` after a delay, and
accept `0xCDD10003` or `0xCDD10001`. Recognising `0xCDD10001` also means being ready to swallow the
ten-word block of §A.6 without mistaking its words for anything else — that is the concrete failure
mode if this is half-implemented. Blocks 6 onwards. **This is the item that turns a booting machine
into one where the game's audio thread runs.**

**6. Do not answer synchronously (§A.7).** Schedule `0xDCD10002` at least a few thousand CPU cycles
after the command list arrives. Cheap to do, and it prevents a hang that looks like anything but an
audio problem.

**7. The audio interface's sample counter (§D.3).** Independent of everything above and needed by
games that pace themselves on it. Already present in `gc_io.c`; the refinement worth making is the
wrapping comparison.

**8. The audio DMA's looping behaviour and its interrupt (§D.1).** The AID interrupt is what drives
the game's audio thread. A game will reach gameplay without it, but its mixer will be starved and
any logic that waits on a buffer refill will stall. Note that the interrupt is `DSPCR_AIINT` in the
*DSP* control register, not the audio interface's own.

Everything below this line produces sound rather than progress, and can be built in any order.

**9. Walking the command list (§B.2, §B.3).** Get the argument counts right first; a walker that
desynchronises is worse than none, because it will execute garbage opcodes. Handle `SETUP`,
`SET_VOICE_LIST`, `PROCESS`, `OUTPUT`, `END`, and skip the rest by their argument counts.

**10. The voice list and the mixer sends (§C.1, §C.5), with `src_type` forced to linear.** Nine
32-bit buffers of 160 samples, five 1 ms sub-frames, `running == 1` to play, null `next_pb` to stop.

**11. The sample decoder (§C.2, §C.3).** ADPCM first — it is what almost every voice uses — and note
that the coefficients come from the parameter block, not from a table you can write down.

**12. `OUTPUT` into the AID buffer (§D.2), 160 frames of interleaved 16-bit stereo.** Check the
channel order against a known-panned sound early; it is unverified here.

**13. Looping (§C.4), the volume envelope's signedness, and the per-millisecond updates (§C.6).**
These are the difference between recognisable audio and correct audio.

Deliberately not on this list, because nothing read here establishes them well enough to implement:
the initial time delay, the de-pop ramp, polyphase resampling without a data-ROM dump, and the
Dolby Pro Logic paths.
