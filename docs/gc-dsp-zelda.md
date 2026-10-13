# The "Zelda" sound microcode

AX (`docs/gc-dsp-ax.md`) is the sound microcode the SDK gives every
developer. Nintendo's own sound library carries a different one, usually
called "Zelda" after the game it was first studied in. It shares nothing
with AX but the mailbox and the ROM's upload protocol, and a game that
uploads it and is answered as AX waits for ever for the second word of the
real greeting.

This is the description `runtime-gc/src/gc_zelda.c` (the conversation) and
`runtime-gc/src/gc_zelda_mix.c` (the sound) are written from, and
`tests/gc/gc_zelda_test.c` and `tests/gc/gc_zelda_mix_test.c` hold them to.

## Provenance

Compiled from Dolphin's DSP-HLE (`Source/Core/Core/HW/DSPHLE/UCodes/Zelda.cpp`,
`UCodes.cpp`, `ROM.cpp`, `MailHandler.cpp`; GPL-2.0-or-later), read as an
account of behaviour. No code is copied: the implementation here is written
from this page.

What stands behind each part:

| Part | Evidence |
|---|---|
| Identification by hash, and the hash of Super Mario Galaxy 2's microcode | **A retail game.** The bytes the game uploads hash to `d643001f`, the value the reference's table gives for it. |
| The greeting, the commands, the acknowledgements, per-sixteen-voices synchronisation, the end of a frame and the game's answer to it | **A retail game.** Super Mario Galaxy 2's own sound library is on the other end and gets from the greeting to rendering thousands of frames, which it could not with a word of this wrong. Members with other traits are from the reference only. |
| Once-a-frame synchronisation, the hand-over to another microcode | The reference only. Tested against this page, not against a game. |
| The early ("light") protocol | Not written. |
| Placed voices, block-coded samples at four bits, resampling, the reverb, the frame's output | **A retail game and a second implementation.** Super Mario Galaxy 2's title music mixed here and by Dolphin is the same to the bit for as long as Dolphin was run: 1,277,378 stereo samples, 39.9 seconds ("Evidence", below). That says the two implementations agree, and that this page says what the reference does; it does not say either is what the hardware does. |
| The other sample sources, voices sent to destinations rather than placed, the two filters, and the traits of members other than Super Mario Galaxy's | The reference only. Tested against this page (`gc_zelda_mix`), not against a game: that music uses none of them. |

There is no hardware oracle for any of it in this repository.

## The family

A microcode is identified by a hash of the bytes the game uploads: each
byte is exclusive-ored into a 32-bit word, which is then rotated left by
three. That is the reference's identification, used here so that the
numbers in logs are the numbers its tables and other people's notes use.

| Hash | Games | Traits |
|---|---|---|
| `24b22038`, `6ba3b3ea` | The GameCube's menu, NTSC and PAL | early protocol |
| `df059f68`, `4be6a5cb` | Pikmin (and its demo), Animal Crossing | early protocol |
| `42f64ac4` | Luigi's Mansion | early protocol |
| `267fd05a` | Pikmin (PAL) | asks once a frame; no command 0D |
| `56d36052` | Super Mario Sunshine | asks once a frame; no command 0D |
| `86840740` | The Wind Waker | |
| `2fcdf1ec` | Four Swords Adventures, Mario Kart: Double Dash, Pikmin 2 | |
| `6ca33a6d` | Twilight Princess (GameCube), Donkey Kong Jungle Beat | render carries two more words |
| `6c3f6f94` | Twilight Princess (Wii) | no audio memory; render carries two more words |
| `d643001f` | Super Mario Galaxy, Super Mario Galaxy 2 | no audio memory; render carries two more words |
| `b7eb9a9c`, `eaeb38cc` | Pikmin and Pikmin 2 reissued for the Wii | no audio memory; render carries two more words |

Anything whose hash is not in the table is taken to be AX, as everything
was before this existed. An early-protocol member is recognised and said
to be unsupported in the log; it is then answered as AX, which it will not
understand.

Other traits change only how a voice sounds ("Mixing a voice", below):
louder placed voices (every member from Four Swords Adventures on), four
destinations instead of six and a volume step given outright (the
GameCube's menu), and a smaller parameter block (the NTSC menu; not
written).

## Mail

The mailbox registers and the DSP interrupt are as `docs/gc-dsp-ax.md`
parts A.2 and A.3 describe. The difference is that this microcode often
owes the game several words at once, and only some of them interrupt.

- Mail is queued. One word is in the mailbox at a time; the next is put
  there after the game has taken the one before.
- A word marked as interrupting raises the DSP interrupt when it reaches
  the mailbox. The others do not, and the game reads them by polling,
  straight after the interrupting word that announced them.
- The same floor on the delay before an answer as AX's applies.

### From the microcode

| Mail | Interrupts | Meaning |
|---|---|---|
| `DCD10000` | yes | The task has started. |
| `F3551111` | no | The family's own greeting, straight after. A game waits for it. |
| `DCD10001` | yes | Resumed, after another microcode had the DSP. |
| `DCD10004` | yes | "Synchronise": a value follows. |
| `F355xxxx` | no | The value: a command's own number (the top half of its first word), or `FFnn` for "frame nn is finished". |
| `DCD10005` | yes | Every frame asked for is finished. |

### From the game

The microcode is in one of four states: waiting, taking a command's words,
taking "which voices", or halted.

Waiting:

| Mail | Meaning |
|---|---|
| top bit set, low half `0001` | Another microcode is to run: ten words follow (below). First, anything waiting to execute does. |
| top bit set, low half `0002` | Back to the ROM. The ROM then says `8071FEED`. |
| top bit set, low half `0003` | Carry on: commands may execute again. |
| top bit set, anything else | Halt. |
| top bit clear, low half zero | "Which voices" follows. Only while frames are being rendered; otherwise halt. |
| top bit clear, low half `n` | A command of `n` words follows. |

The real microcode looks only at the low half of a word with the top bit
set; games send `CDD1` in the top half.

## Commands

A command is `n` words. The first has the top bit set, the command's number
in bits 24 to 30, and the rest is its own. Commands execute in the order
they arrive, but not while frames are being rendered and not between the
end of the frames and the game's answer to it; they wait.

Every command but render is acknowledged with `DCD10004` and then `F355` and
the top half of its first word.

| Number | Words after the first | What it does |
|---|---|---|
| `00`, `03`, `0A`, `0B`, `0C`, `0F` | none | Nothing (for the members spoken to here). |
| `01` | 4 | Set-up. The low half of the first word is the number of voices in a frame. Then: the address of the voices' parameter blocks; the address of the tables (resampling coefficients at +0, wave patterns at +0x200, a sine table at +0x400); the address of the sample decoder's coefficients; the address of the reverb's blocks. |
| `02` | 2, or 4 with the "two more words" trait | Render. Bits 16 to 23 of the first word are the number of frames and the low half the output volume. Then the addresses the left and the right samples go to. The two extra words are ignored. Not acknowledged; see below. |
| `0D` | 1, or none on members without it | Not known. |
| `0E` | 1 | On a console with no audio memory: where in main memory the samples that would have been in it are. |
| `04` to `09` | | Halt. They crash the real microcode. |

## Rendering

A frame is eighty samples for each of two channels. The render command
starts the frames and nothing after it executes until they are done.

For each frame the microcode goes through the voices in order, and stops
when it reaches a voice the game has not yet spoken for. The game speaks
for voices with a word of zero ("which voices" follows) and then:

- **Per sixteen** (most members): one word. Bits 16 to 23 say which
  sixteen, and the low half has a bit for each, the first voice in the top
  bit; a clear bit skips the voice. The microcode renders up to the end of
  that sixteen and waits again.
- **Once a frame** (the two members marked so): two words, thirty-two
  voices each, after one word of zero. The frame is rendered after the
  second.

When the last voice of a frame is done the microcode sends `DCD10004` and
`F355FFnn`, writes the frame - the left's eighty samples to the left
address and the right's to the right, sixteen bits each, big-endian - and
moves both addresses on by a frame. A frame of no voices needs no asking.

After the last frame it sends `DCD10005`, and executes nothing more until
the game answers (`0003` to carry on, `0001` to run another microcode).

## Another microcode

After `0001` the game sends ten words: where the microcode's data memory is
to be saved (address, length, source), the new microcode's code (its
address in main memory, its length, where it goes, where it starts), and
its data (address, length, destination). On a GameCube this is how the
memory card's unlock microcode borrows the DSP.

What is written: if the new microcode is the one already running, it is
told it has resumed (`DCD10001`). Anything else is started from nothing, as
an upload through the ROM would start it, and the log says so.

## A voice

Everything from here to "Evidence" is `gc_zelda_mix.c`.

A voice is a block of 0xC0 sixteen-bit words, big-endian, in the game's
memory; voice `n`'s is at the set-up command's first address plus
`n * 0x180`. The first 0x80 words are the voice's state, which the microcode
reads before mixing the voice and writes back after; the rest the game sets
and the microcode only reads. Words not listed are not used here.

| Word | What |
|---|---|
| `00` | Enabled. A voice that is not is passed over. |
| `01` | Done. Set by the microcode when the voice has ended; a voice that is done is passed over. This is how a game knows a sound has finished. |
| `02` | Ratio, 4.12: raw samples for each output sample. |
| `04` | Reset: start from the beginning. Cleared after the voice's first frame, unless it is only holding a sample. |
| `05` | The samples' end was reached (8- and 16-bit sources). |
| `06` | Hold: every sample of the frame is word `33`. |
| `08` to `1F` | Six destinations of four words: a buffer, a target volume, the volume now, unused. |
| `28` | Position, for a placed voice: left to right in bits 8 to 14, front to back in bits 0 to 6. |
| `29` | Placed: how much goes to the reverb as well, 1.15. |
| `2A`, `2B` | Placed: the volume now, and its target. |
| `2C` | Placed (non-zero) or sent to destinations. |
| `30` | How far between two raw samples the resampler is, 0.12. |
| `32` | How many decoded samples of the last block are still to be given out. |
| `33` | The last sample the voice produced. |
| `34`, `35` | Position, in samples. |
| `36` | Samples before the loop (source 33). |
| `38`, `39` | Where in sample memory the voice has got to. |
| `3A`, `3B` | Samples remaining before the end. |
| `3C` to `3F` | The four raw samples before the next frame's first. |
| `54` to `57` | The two-pole filter's last two inputs and last two outputs. |
| `58` to `67` | The last block decoded, sixteen samples; the last two are what the next block is decoded from. |
| `68`, `69` | The low-pass filter's last output and last input. |
| `80` | The sample source (below). |
| `81` | Looping. |
| `82`, `83` | The two samples before the loop's start, newest first, for the block decoder. |
| `84` | Bit 5: the two-pole filter is on. Bits 0 to 4: a variable filter, not written. |
| `85` | The game has asked the voice to stop. |
| `88`, `89` | Where the loop goes back to, in samples. |
| `8A`, `8B` | Where the samples end (and the loop is taken from), in samples. |
| `8C`, `8D` | Where the samples start: an offset into sample memory, or for source 33 an address in main memory. |
| `A4` to `A7` | The two-pole filter's coefficients, 1.15: for the input before, the one before that, the output before, the one before that. |
| `A8` | The low-pass filter's coefficient; zero is off. |

Sample memory is the GameCube's audio memory. A Wii has none: its members
read main memory, from the address command `0E` gave.

## A voice's samples

A frame needs `(word 30 + 0x50 * ratio) >> 12` raw samples. They come from
the source, after the four the voice kept from the frame before, and the
resampler turns them into eighty.

| Source | What |
|---|---|
| 0, 3 | A square wave, 0x4000 high and -0x4000 low, high half the time or a quarter of it. Not resampled: a position kept shifted left by one (by two at a quarter) steps by the ratio (by twice it), and the sample is high while bit 16 (bits 16 and 17) of it is clear. |
| 1 | A sawtooth: the position itself, stepping by half the ratio. |
| 7, 4, 11, 12 | The four wave patterns of sixty-four samples from the tables, stepped through at the ratio shifted left five (position kept shifted left six), going round. Not resampled. |
| 10 | The first pattern, its position pushed each sample by the back right buffer's sample times the ratio. |
| 8, 16 | Plain samples, eight bits (shifted up to sixteen) or sixteen. On reset the samples remaining are the end less the position and the address is the start plus the position. When they run out the voice either is done, the rest of the frame silence, or goes back to the loop's position. |
| 9, 5 | Block-coded samples, sixteen to a block of nine bytes (four bits each) or five (two bits each). |
| 33 | Sixteen-bit samples in main memory, by position. At the end the last sample is held and the voice is done; a loop moves the start to another address. |
| 2, 6 | Exist; not described by anybody. Silent here, and counted. |

**Block-coded.** A block's first byte has a shift in its top four bits and
one of sixteen coefficient pairs in the bottom four. Each code is taken as a
signed sixteen-bit number with the code in its top bits, halved; a sample is
`(code << shift) + before * pair[0] + before_that * pair[1]`, all shifted
right eleven and clamped to sixteen bits. Blocks are decoded whole. What a
frame did not need of the last block is kept (words `58` on, count in `32`)
and given out first next time. On reset the two samples before are zero.
When fewer samples remain than a frame needs, what remains is decoded, and
then the voice is done - or, looping, the block holding the loop's position
is decoded from words `82` and `83`, and the part of it before the loop is
skipped.

**Resampling.** Below four raw samples to one, each output sample is four
consecutive raw samples times four coefficients (1.15), summed and clamped;
which four coefficients is the top six bits of the position's fraction
(sixty-four sets, at the start of the tables). From four to one up, the
nearest raw sample is taken. The four raw samples after the last used are
kept for the next frame, the last output sample goes in word `33`, and the
fraction is kept.

## Mixing a voice

The voice's eighty samples first go through its filters. The low-pass,
when its coefficient is not zero: `out = ((in - in_before) * coefficient >> 7) + out_before`,
clamped; reset clears its history. The two-pole filter, when on and not the
one that changes nothing: the four coefficients times the two inputs before
and the two outputs before, summed, shifted right fifteen and clamped.

Thirteen buffers of eighty samples can be named, by where they are in the
real microcode's data memory:

| Number | Buffer | | Number | Buffer |
|---|---|---|---|---|
| `0D00` | front left | | `0D60` | front right |
| `0F40` | back left | | `0CA0` | back right |
| `0E80` | front left reverb | | `0EE0` | front right reverb |
| `0C00` | back left reverb | | `0C50` | back right reverb |
| `0DC0`, `0E20` | two more reverb lines | | `09A0`, `0FA0`, `0B00` | three others |

A sum that does not fit sixteen bits wraps; it is not clamped.

**Sent.** Each of the six destinations (four on the menu's members) with a
buffer number gets the samples at a volume that moves from "now" to the
target in eighty equal steps of 16.16, only the whole part multiplying and
the product shifted right sixteen. (On the member with the explicit-step
trait the target word is the whole frame's change instead.) The volume it
reached is written back. Asked to stop, every target becomes half the volume
now, and when all are nothing the voice is done.

**Placed.** One volume and a position. A quarter sine of 128 entries (in the
tables) turns the left-to-right position `x` into two gains, `sine[x]` for
the right and `sine[127 - x]` for the left, and the front-to-back position
likewise into back and front. Their four products are the corners' gains -
shifted right fifteen on the louder members, sixteen on the others. Each
corner's gain times the volume now is where its ramp starts and times the
difference to the target is how far it moves across the frame; each corner
also has a reverb buffer, which gets the same ramp scaled by word `29`. The
target becomes the volume now. Asked to stop, the target is half the volume
now, and at nothing the voice is done.

## A frame

Before the first voice: the front pair is cleared; the back pair is scaled
by 0x6784 (1.15) rather than cleared; the reverb lines give back what they
held (below); the back reverb pair is added to the front reverb pair (the
left whole to the left and at 0xB820 to the right, the second half of the
right the other way round) and cleared; and two of the wave patterns, which
are generators rather than waves, make their next sixty-four samples from
their last two.

After the last voice: the front pair is scaled by the render command's
volume (4.12, clamped) and written out, and the reverb lines take in what
the voices sent them.

The real microcode also filters the back pair and folds it into the front
pair before writing. The reference does not, and neither does this: what a
voice sends behind is not heard.

## The reverb

Four blocks of sixteen words at the set-up command's last address, one for
each of four delay lines: the two extra reverb buffers, then front left
reverb and front right reverb.

| Word | What |
|---|---|
| `0` | On, when not zero. Bit 0: filter before feeding the destinations. Bit 1: filter after. |
| `1` | How many frames long the line's ring is. |
| `2`, `3` | Where the ring is, in main memory. |
| `4` to `7` | Two destinations: a buffer number (zero for none) and a volume, 1.15. |
| `8` to `F` | Eight filter coefficients, 1.15. |

Before the voices, the frame the ring is at is read; filtered, if bit 0 says
so, each sample becoming the sum of the eight samples before it times the
coefficients (the eight before a frame's first are the last eight of the
frame read the time before); added to each destination at its volume;
filtered, if bit 1 says so; and put in the line's buffer, where the voices
add to it. After the voices, the buffer is written into the ring at the same
place and the ring moves on. What a voice sends to a line is therefore heard
a whole ring later, and goes round again.

## Evidence

`tools/wii/oracle_dolphin_game.py --audio` runs the same disc under Dolphin
and brings back what the game gave the audio interface;
`tools/wii/compare_audio.py` lines that up with the runner's `--dump-audio`
by content and compares sample for sample.

Super Mario Galaxy 2 (`SB4E01`), left at its title screen, 2026-10-07:
identical to the bit for all 1,277,378 stereo samples (39.9 s) that Dolphin
produced from the first note. In the run that was compared (4,500 fields)
the game had 73,602 voice-frames mixed, every one of them placed, every one
block-coded at four bits (source 9), with all four reverb lines on
throughout. So that - and the
resampler, and the frame's set-up and output - is what the comparison
covers, and what the "A retail game and a second implementation" row above
means. Nothing in it used a voice sent to destinations, another source, or
a filter; those are held to this page by `gc_zelda_mix` alone. (Through
its opening and into its prologue the same game does send voices to
destinations - 63,526 voice-frames of 260,663 in a 6,000-field scripted
run - and plays sixteen-bit samples, source 16, beside source 9. That has
run, its dump has no sample at the limit and no jump of half the range
between neighbours, and it has not been compared with anything or listened
to.)

The runner's summary line says which sources, how many placed voices and
how many reverb line-frames a run used, so that a game that reaches for
something this comparison did not cover says so.

## What is not written

- **The early protocol**, which five microcodes speak, and with it the
  small parameter block of the NTSC menu's.
- **Sample sources 2 and 6**, and the variable filter in word `84`.
- **The back channels' fold-down** into the front pair: a placed voice
  behind the listener loses that part of itself.
- **GameCube members are untried.** The Wind Waker's, Super Mario
  Sunshine's and the others' microcodes now mix as this page says, from
  audio memory, and no game that uploads one has been run.
- **Saving a data memory** on the hand-over, and returning to a microcode
  other than the one that handed over.
