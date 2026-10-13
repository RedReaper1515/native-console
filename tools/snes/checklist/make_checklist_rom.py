#!/usr/bin/env python3
"""Build the SNES assembly-to-C checklist test ROM.

Each test below exercises one or more items of the SNES assembly-to-C
nuances checklist (docs/SNES-PLAN.md, "Checklist coverage") on the machine
itself: a short 65C816 routine whose result is checked in the ROM. The ROM
runs every test in order and records a result byte per test in work RAM:

    $7E:2000 + n   $01 pass, $FF fail, $00 not reached
    $7E:2100       $D0 once every test has run; $7E:2101 = test count

The same ROM is run by the interpreter (snes_run), recompiled by
snesrecomp, and run again natively and in --differential lockstep
(tools/snes/run_checklist.py), so each test checks the interpreter and the
generated C against the hardware behaviour the test encodes.

Expected values come from hardware documentation (fullsnes), from gilyon's
hardware-verified cputest where it covers the case, or are computed here
in Python from the algorithm the test runs (PRNG, fixed point). Tests that
measure timing check relations (FastROM is faster than SlowROM) rather than
exact counts.

    python tools/snes/checklist/make_checklist_rom.py -o checklist.sfc [--json tests.json]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from asm65816 import AsmError, Assembler  # noqa: E402

ROM_SIZE = 0x20000  # 128 KiB LoROM: banks $00-$03


def lfsr16(seed: int, steps: int) -> int:
    """Galois LFSR, taps $B400: shift right, XOR the taps when a 1 falls out."""
    v = seed
    for _ in range(steps):
        lsb = v & 1
        v >>= 1
        if lsb:
            v ^= 0xB400
    return v


def m7_product(a16: int, b8: int) -> int:
    a = a16 - 0x10000 if a16 & 0x8000 else a16
    b = b8 - 0x100 if b8 & 0x80 else b8
    return (a * b) & 0xFFFFFF


LFSR_EXPECT = lfsr16(0xACE1, 100)
M7_1 = m7_product(0x1234, 0x56)
M7_2 = m7_product(0x3412, 0x56)
M7_3 = m7_product(0xFEDC, 0x9A)

# (nuance numbers, description, body). The body runs with A 8-bit, X/Y
# 16-bit, D = 0, DBR = 0, native mode, interrupts off, forced blank; it
# branches to @fail on a mismatch. '@' is replaced by the test's label.
# saved_s ($0082) holds S before the driver's JSR: a test that moves S
# restores it to saved_s - 2 before returning. Entering emulation mode
# forces S's high byte to $01, so tests save S ($70) around it.
TESTS: list[tuple[list[int], str, str]] = [
    ([1, 50], "REP/SEP switch the accumulator width; an 8-bit load keeps the high byte", r"""
    rep #$20
    lda #$1234
    cmp #$1234
    bne @fail
    sep #$20
    lda #$56
    rep #$20
    cmp #$1256
    bne @fail
"""),
    ([2, 48], "8-bit A keeps B; TAX copies 16 bits when X is 16-bit, 8 when X is 8-bit", r"""
    rep #$20
    lda #$ABCD
    sep #$20
    lda #$12
    xba
    cmp #$AB
    bne @fail
    xba
    tax
    cpx #$AB12
    bne @fail
    sep #$10
    tax
    rep #$10
    cpx #$0012
    bne @fail
"""),
    ([3], "Direct page follows D (D = $0300, and D = $0301 with a non-zero low byte)", r"""
    rep #$20
    lda #$0300
    tcd
    sep #$20
    lda #$5A
    sta $10
    rep #$20
    lda #$0000
    tcd
    sep #$20
    lda $0310
    cmp #$5A
    bne @fail
    lda #$A5
    sta $0311
    rep #$20
    lda #$0301
    tcd
    sep #$20
    lda $10
    pha
    rep #$20
    lda #$0000
    tcd
    sep #$20
    pla
    cmp #$A5
    bne @fail
"""),
    ([4], "Absolute data addresses use the data bank register", r"""
    lda #$11
    sta.l $7E4000
    lda #$22
    sta.l $7F4000
    lda #$7F
    pha
    plb
    lda $4000
    sta.l $7E0000
    lda #$7E
    pha
    plb
    lda $4000
    sta.l $7E0001
    lda #$00
    pha
    plb
    lda $00
    cmp #$22
    bne @fail
    lda $01
    cmp #$11
    bne @fail
"""),
    ([5, 37, 38], "JSL sets PBR, PHK sees it, RTL pops three bytes; JSR/RTS two", r"""
    tsx
    stx $00
    jsl far_phk
    cmp #$01
    bne @fail
    tsx
    cpx $00
    bne @fail
    jsr near_depth
    jsl far_depth
    rep #$20
    lda $00
    sec
    sbc $04
    cmp #$0002
    bne @fail
    lda $00
    sec
    sbc $06
    cmp #$0003
    bne @fail
"""),
    ([6, 40, 90], "24-bit addresses; long,X and abs,X carry into the next bank", r"""
    lda #$33
    sta.l $7F0000
    lda #$44
    sta.l $7EFFFF
    ldx #$0001
    lda.l $7EFFFF,x
    cmp #$33
    bne @fail
    lda #$7E
    pha
    plb
    lda $FFFF,x
    sta.l $7E0000
    lda #$00
    pha
    plb
    lda $00
    cmp #$33
    bne @fail
"""),
    ([7, 47], "8/16-bit wraparound of A and the index registers", r"""
    rep #$20
    lda #$FFFF
    inc a
    bne @fail
    sep #$20
    lda #$FF
    clc
    adc #$01
    bne @fail
    bcc @fail
    sep #$10
    ldx #$FF
    inx
    bne @fail
    rep #$10
    ldx #$FFFF
    inx
    bne @fail
"""),
    ([8, 33], "Carry is an input to ADC/SBC; a 32-bit add chains it", r"""
    clc
    lda #$10
    adc #$05
    cmp #$15
    bne @fail
    sec
    lda #$10
    adc #$05
    cmp #$16
    bne @fail
    sec
    lda #$10
    sbc #$05
    cmp #$0B
    bne @fail
    clc
    lda #$10
    sbc #$05
    cmp #$0A
    bne @fail
    rep #$20
    clc
    lda #$FFFF
    adc #$0001
    sta $00
    lda #$0001
    adc #$0000
    sta $02
    lda $00
    bne @fail
    lda $02
    cmp #$0002
    bne @fail
"""),
    ([9], "Decimal mode ADC/SBC, 8 and 16 bits, with carry out", r"""
    sed
    clc
    lda #$19
    adc #$28
    cld
    cmp #$47
    bne @fail
    sed
    sec
    lda #$50
    sbc #$01
    cld
    cmp #$49
    bne @fail
    sed
    clc
    lda #$99
    adc #$01
    cld
    bcc @fail
    cmp #$00
    bne @fail
    rep #$20
    sed
    clc
    lda #$1999
    adc #$0001
    cld
    cmp #$2000
    bne @fail
"""),
    ([10], "Overflow (V) and carry (C) differ", r"""
    rep #$20
    clc
    lda #$7FFF
    adc #$0001
    bvc @fail
    bcs @fail
    bpl @fail
    clc
    lda #$FFFF
    adc #$0001
    bvs @fail
    bcc @fail
    sep #$20
    clc
    lda #$7F
    adc #$01
    bvc @fail
"""),
    ([11, 12], "N and Z come from the active width", r"""
    rep #$20
    lda #$0080
    bmi @fail
    sep #$20
    lda #$80
    bpl @fail
    rep #$20
    lda #$1234
    sep #$20
    lda #$00
    bne @fail
    rep #$20
    lda #$1200
    beq @fail
"""),
    ([13, 75], "Compares set flags only; unsigned (C) and signed (N xor V) outcomes", r"""
    lda #$40
    cmp #$41
    bcs @fail
    cmp #$40
    bne @fail
    cmp #$3F
    bcc @fail
    cmp #$40
    bne @fail
    lda #$7F
    cmp #$80
    bcs @fail
    sec
    sbc #$80
    bvc @nv
    eor #$80
@nv:
    bmi @fail
    ldx #$1234
    cpx #$1235
    bcs @fail
    cpx #$1234
    bne @fail
"""),
    ([14], "BIT copies N and V from memory; BIT # changes Z only", r"""
    lda #$C0
    sta $10
    lda #$01
    bit $10
    bne @fail
    bpl @fail
    bvc @fail
    bit #$01
    beq @fail
    bpl @fail
    bvc @fail
    rep #$20
    lda #$4000
    sta $10
    bit $10
    beq @fail
    bmi @fail
    bvc @fail
"""),
    ([15, 81], "ROL/ROR use the carry; shifts build bit streams in order", r"""
    clc
    lda #$81
    rol a
    bcc @fail
    cmp #$02
    bne @fail
    sec
    lda #$01
    ror a
    bcc @fail
    cmp #$80
    bne @fail
    lda #$B1
    stz $12
    ldy #$0008
@l1:
    asl a
    rol $12
    dey
    bne @l1
    lda $12
    cmp #$B1
    bne @fail
    lda #$B1
    stz $12
    ldy #$0008
@l2:
    lsr a
    rol $12
    dey
    bne @l2
    lda $12
    cmp #$8D
    bne @fail
"""),
    ([16, 17], "XCE enters emulation mode: M/X forced, stack wraps within page 1", r"""
    lda #$00
    sta $0100
    sec
    xce
.i8
    php
    pla
    and #$30
    cmp #$30
    bne @efail
    ldx #$00
    txs
    lda #$5A
    pha
    tsx
    stx $14
    clc
    xce
    rep #$10
    ldx $0082
    dex
    dex
    txs
    lda $0100
    cmp #$5A
    bne @fail
    lda $14
    cmp #$FF
    bne @fail
    bra @done
@efail:
    clc
    xce
    rep #$10
    ldx $0082
    dex
    dex
    txs
    bra @fail
@done:
"""),
    ([18, 19, 20], "PHA/PLA and PHX/PLX move 1 or 2 bytes by width", r"""
    tsx
    stx $00
    lda #$11
    pha
    rep #$20
    lda #$2233
    pha
    tsc
    sta $02
    lda $00
    sec
    sbc $02
    cmp #$0003
    bne @fail
    pla
    cmp #$2233
    bne @fail
    sep #$20
    pla
    cmp #$11
    bne @fail
    sep #$10
    ldx #$44
    phx
    rep #$10
    ldx #$5566
    phx
    plx
    cpx #$5566
    bne @fail
    sep #$10
    plx
    cpx #$44
    bne @fail
    rep #$10
"""),
    ([21, 51], "COP pushes PBR/PC/P; the handler changes widths; RTI restores M/X", r"""
    stz $8A
    stz $8B
    rep #$20
    cop #$42
    php
    sep #$20
    pla
    and #$20
    bne @fail
    lda $8A
    cmp #$01
    bne @fail
    lda $8B
    cmp #$42
    bne @fail
"""),
    ([22, 56, 84], "NMI arrives in V-blank once per frame; the handler counts frames", r"""
    lda #$80
    sta $4200
    rep #$20
    lda $0084
    clc
    adc #$0003
    sta $00
    ldy #$0000
@wait:
    dey
    beq @timeout
    lda $0084
    cmp $00
    bne @wait
    sep #$20
    stz $4200
    lda $008E
    and #$80
    beq @fail
    bra @done
@timeout:
    sep #$20
    stz $4200
    bra @fail
@done:
"""),
    ([23, 86], "DMA: to VRAM and back, fixed-source fill, channel order, no WRAM-to-WRAM", r"""
    lda #$80
    sta $2115
    ldx #$1000
    stx $2116
    lda #$01
    sta $4300
    lda #$18
    sta $4301
    ldx #dma_src
    stx $4302
    lda #$00
    sta $4304
    ldx #$0008
    stx $4305
    lda #$01
    sta $420B
    ldx #$1000
    stx $2116
    lda #$81
    sta $4300
    lda #$39
    sta $4301
    ldx #$0200
    stx $4302
    lda #$7E
    sta $4304
    ldx #$000A
    stx $4305
    lda #$01
    sta $420B
    ldx #$0000
@cmp:
    lda $0200,x
    cmp dma_readback,x
    bne @fail
    inx
    cpx #$000A
    bne @cmp
    ldx #$0300
    stx $2181
    stz $2183
    lda #$08
    sta $4300
    sta $4310
    lda #$80
    sta $4301
    sta $4311
    ldx #dma_a
    stx $4302
    ldx #dma_b
    stx $4312
    lda #$00
    sta $4304
    sta $4314
    ldx #$0004
    stx $4305
    stx $4315
    lda #$03
    sta $420B
    ldx #$0000
@cmp2:
    lda $0300,x
    cmp dma_expect,x
    bne @fail
    inx
    cpx #$0008
    bne @cmp2
    lda #$EE
    sta $0310
    ldx #$0310
    stx $2181
    lda #$00
    sta $4300
    lda #$80
    sta $4301
    ldx #$0400
    stx $4302
    lda #$7E
    sta $4304
    ldx #$0001
    stx $4305
    lda #$01
    sta $420B
    lda $0310
    cmp #$EE
    bne @fail
"""),
    ([24], "HDMA transfers one table entry per line (to WRAM through $2180)", r"""
    ldx #$0000
@clr:
    stz $0400,x
    inx
    cpx #$0010
    bne @clr
    jsr wait_vblank
    ldx #$0400
    stx $2181
    stz $2183
    lda #$00
    sta $4370
    lda #$80
    sta $4371
    ldx #hdma_table
    stx $4372
    lda #$00
    sta $4374
    lda #$80
    sta $420C
    jsr wait_vblank_end
    jsr wait_vblank
    stz $420C
    ldx #$0000
@cmp:
    lda $0400,x
    cmp hdma_expect,x
    bne @fail
    inx
    cpx #$0006
    bne @cmp
"""),
    ([25, 26], "VRAM port: address increments on the selected byte, by 1 or 32; the read latch lags a word", r"""
    stz $2115
    ldx #$2000
    stx $2116
    lda #$01
    sta $2118
    lda #$02
    sta $2118
    lda #$80
    sta $2115
    ldx #$2000
    stx $2116
    lda $2139
    cmp #$01
    bne @fail
    lda $213A
    lda $2139
    cmp #$01
    bne @fail
    lda $213A
    lda $2139
    cmp #$02
    bne @fail
    lda #$81
    sta $2115
    ldx #$2100
    stx $2116
    lda #$0A
    sta $2118
    lda #$0B
    sta $2119
    lda #$0C
    sta $2118
    lda #$0D
    sta $2119
    lda #$80
    sta $2115
    ldx #$2120
    stx $2116
    lda $2139
    cmp #$0C
    bne @fail
    lda $213A
    cmp #$0D
    bne @fail
"""),
    ([27], "CGRAM: two writes per 15-bit color, bit 15 dropped, incomplete pairs not stored", r"""
    lda #$05
    sta $2121
    lda #$FF
    sta $2122
    sta $2122
    lda #$05
    sta $2121
    lda $213B
    cmp #$FF
    bne @fail
    lda $213B
    and #$7F
    cmp #$7F
    bne @fail
    lda #$06
    sta $2121
    lda #$00
    sta $2122
    sta $2122
    lda #$06
    sta $2121
    lda #$12
    sta $2122
    lda #$06
    sta $2121
    lda $213B
    bne @fail
"""),
    ([28], "OAM: low table pairs and the high table at $200", r"""
    stz $2102
    lda #$01
    sta $2103
    lda #$5A
    sta $2104
    stz $2102
    lda #$01
    sta $2103
    lda $2138
    cmp #$5A
    bne @fail
    stz $2102
    stz $2103
    lda #$11
    sta $2104
    lda #$22
    sta $2104
    stz $2102
    stz $2103
    lda $2138
    cmp #$11
    bne @fail
    lda $2138
    cmp #$22
    bne @fail
"""),
    ([30, 31], "TSB/TRB keep unrelated bits; an RMW on a register writes it twice in emulation mode", r"""
    lda #$F0
    sta $20
    lda #$0F
    tsb $20
    bne @fail
    lda $20
    cmp #$FF
    bne @fail
    lda #$3C
    trb $20
    lda $20
    cmp #$C3
    bne @fail
    lda #$77
    sta $0500
    stz $0501
    stz $0502
    lda #$33
    sta $0600
    stz $0601
    stz $0602
    ldx #$0500
    stx $2181
    stz $2183
    tsx
    stx $70
    sec
    xce
    inc $2180
    clc
    xce
    rep #$10
    ldx $70
    txs
    ldx #$0600
    stx $2181
    inc $2180
    lda $0501
    cmp #$77
    bne @fail
    lda $0502
    cmp #$78
    bne @fail
    lda $0601
    cmp #$34
    bne @fail
    lda $0602
    bne @fail
"""),
    ([32, 77, 78, 79, 80], "Routines take A, return in carry and Y, and preserve X with PHX/PLX", r"""
    lda #$07
    jsr is_above5
    bcc @fail
    lda #$03
    jsr is_above5
    bcs @fail
    ldx #$1234
    jsr clobber_keep_x
    cpx #$1234
    bne @fail
    cpy #$0099
    bne @fail
"""),
    ([34, 35], "Branches use the flags of the instruction that set them (INC/DEX leave C)", r"""
    sec
    lda #$FF
    inc a
    bne @fail
    bcc @fail
    clc
    ldx #$0000
    dex
    bcs @fail
    bpl @fail
"""),
    ([36, 42, 53], "A branch at the end of a bank wraps within it (into a WRAM mirror)", r"""
    lda #$4B
    sta $0040
    lda #$68
    sta $0041
    lda #$85
    sta $0042
    lda #$48
    sta $0043
    lda #$6B
    sta $0044
    stz $48
    jsl $01FFFE
    lda $48
    cmp #$01
    bne @fail
"""),
    ([39, 65, 69, 70], "Indirect modes: (dp),Y [dp],Y (dp,X) JMP (a) JMP (a,X) JML [a]; E-mode dp wrap", r"""
    ldx #$0300
    stx $10
    lda #$9A
    sta $0305
    ldy #$0005
    lda ($10),y
    cmp #$9A
    bne @fail
    lda #$00
    sta $14
    lda #$10
    sta $15
    lda #$7F
    sta $16
    lda #$6C
    sta.l $7F1002
    ldy #$0002
    lda [$14],y
    cmp #$6C
    bne @fail
    ldx #$0310
    stx $22
    lda #$4D
    sta $0310
    ldx #$0002
    lda ($20,x)
    cmp #$4D
    bne @fail
    ldx #far_ret1
    stx $0050
    jsl far_jmpind
    cmp #$01
    bne @fail
    jsl far_jmpindx
    cmp #$02
    bne @fail
    ldx #far_ret3
    stx $0054
    lda #$01
    sta $0056
    jml [$0054]
@back:
    cmp #$03
    bne @fail
    lda #$00
    sta $00FF
    lda #$03
    sta $0000
    lda #$04
    sta $0100
    ldy #$0003
    lda #$E1
    sta $0403
    lda #$E2
    sta $0303
    tsx
    stx $70
    sec
    xce
.i8
    ldy #$03
    lda ($FF),y
    clc
    xce
    rep #$10
    ldx $70
    txs
    cmp #$E2
    bne @fail
"""),
    ([41, 42], "Code copied to WRAM runs, and runs again after its bytes change", r"""
    lda #$A9
    sta $1000
    lda #$11
    sta $1001
    lda #$60
    sta $1002
    jsr $1000
    cmp #$11
    bne @fail
    lda #$22
    sta $1001
    jsr $1000
    cmp #$22
    bne @fail
    ldy #$0003
@loop:
    lda $1001
    inc a
    sta $1001
    jsr $1000
    cmp $1001
    bne @fail
    dey
    bne @loop
    lda #$A2
    sta $1000
    lda #$25
    sta $1001
    lda #$00
    sta $1002
    lda #$60
    sta $1003
    lda #$00
    jsr $1000
    cmp #$00
    bne @fail
    cpx #$0025
    bne @fail
"""),
    ([43, 44], "FastROM code runs more loop iterations per frame than SlowROM; WRAM equals SlowROM", r"""
    lda #$80
    sta $4200
    stz $420D
    jsr count_slow
    stx $30
    jsl count_fast
    stx $32
    jsr count_wram_setup
    jsr $1100
    stx $34
    stz $4200
    rep #$20
    lda $32
    cmp $30
    bcc @fail
    beq @fail
    lda $34
    sec
    sbc $30
    bpl @pos
    eor #$FFFF
    inc a
@pos:
    cmp #$0003
    bcs @fail
"""),
    ([45, 82], "Hardware multiply/divide results, divide by zero, and the partial product mid-way", r"""
    lda #$12
    sta $4202
    lda #$34
    sta $4203
    nop
    nop
    nop
    nop
    ldx $4216
    cpx #$03A8
    bne @fail
    ldx #$1234
    stx $4204
    lda #$10
    sta $4206
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    ldx $4214
    cpx #$0123
    bne @fail
    ldx $4216
    cpx #$0004
    bne @fail
    ldx #$4321
    stx $4204
    stz $4206
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    ldx $4214
    cpx #$FFFF
    bne @fail
    ldx $4216
    cpx #$4321
    bne @fail
    lda #$12
    sta $4202
    lda #$34
    sta $4203
    lda $4216
    cmp #$68
    bne @fail
"""),
    ([46, 62], "No implicit sign extension; arithmetic shift rounds down, LSR truncates", r"""
    lda #$FF
    rep #$20
    and #$00FF
    cmp #$00FF
    bne @fail
    sep #$20
    lda #$FE
    rep #$20
    and #$00FF
    bit #$0080
    beq @ext
    ora #$FF00
@ext:
    cmp #$FFFE
    bne @fail
    sep #$20
    lda #$FA
    cmp #$80
    ror a
    cmp #$FD
    bne @fail
    cmp #$80
    ror a
    cmp #$FE
    bne @fail
    lda #$F7
    lsr a
    lsr a
    lsr a
    cmp #$1E
    bne @fail
"""),
    ([60, 61, 63, 64], "A 16-bit LFSR, 8.8 fixed point, and an 8-bit table index that wraps", """
    rep #$20
    lda #$ACE1
    ldy #100
@lfsr:
    lsr a
    bcc @nt
    eor #$B400
@nt:
    dey
    bne @lfsr
    cmp #${lfsr:04X}
    bne @fail
    lda #$0000
    ldy #10
@acc:
    clc
    adc #$0180
    dey
    bne @acc
    cmp #$0F00
    bne @fail
    sep #$20
    sep #$10
    ldx #$FE
    inx
    inx
    lda table256,x
    rep #$10
    cmp #$A0
    bne @fail
""".replace("{lfsr:04X}", f"{LFSR_EXPECT:04X}")),
    ([66, 67, 68, 72, 73, 74], "Little-endian words at odd addresses; bytes and words alias; XBA", r"""
    rep #$20
    lda #$1234
    sta $0301
    sep #$20
    lda $0301
    cmp #$34
    bne @fail
    lda $0302
    cmp #$12
    bne @fail
    lda #$AB
    sta $0302
    rep #$20
    lda $0301
    cmp #$AB34
    bne @fail
    xba
    cmp #$34AB
    bne @fail
"""),
    ([52, 54], "Write-only and unmapped addresses read open bus; RDNMI's version bits", r"""
    lda $2100
    cmp #$21
    bne @fail
    lda $5000
    cmp #$50
    bne @fail
    lda.l $005000
    cmp #$00
    bne @fail
    lda $4210
    and #$0F
    cmp #$02
    bne @fail
"""),
    ([53, 55], "WRAM and ROM mirrors; LoROM bank layout", r"""
    lda #$99
    sta $0123
    lda.l $7E0123
    cmp #$99
    bne @fail
    lda.l $800123
    cmp #$99
    bne @fail
    lda.l $008000
    cmp.l $808000
    bne @fail
    cmp #$78
    bne @fail
    lda.l $018000
    cmp #$4B
    bne @fail
    lda.l $408000
    cmp #$78
    bne @fail
"""),
    ([57, 58], "APU boot handshake, upload and run a program, then a command/acknowledge round trip", r"""
    ldy #$0000
@ready:
    dey
    beq @fail1
    ldx $2140
    cpx #$BBAA
    bne @ready
    ldx #$0200
    stx $2142
    lda #$01
    sta $2141
    lda #$CC
    sta $2140
    ldy #$0000
@ack:
    dey
    beq @fail1
    cmp $2140
    bne @ack
    ldx #$0000
@byte:
    lda spc_prog,x
    sta $2141
    txa
    sta $2140
    ldy #$0000
@echo:
    dey
    beq @fail1
    cmp $2140
    bne @echo
    inx
    cpx #$0007
    bne @byte
    ldx #$0200
    stx $2142
    stz $2141
    lda #$08
    sta $2140
    ldy #$0000
@start:
    dey
    beq @fail1
    cmp $2140
    bne @start
    lda #$41
    sta $2140
    ldy #$0000
@reply:
    dey
    beq @fail1
    lda $2141
    cmp #$42
    bne @reply
    bra @done
@fail1:
    bra @fail
@done:
"""),
    ([59, 85], "Auto-joypad read with scripted input: new presses counted once; serial read agrees", r"""
    lda #$81
    sta $4200
    rep #$20
    stz $36
    ldy #$0800
@frame:
    jsr wait_vblank_end
    jsr wait_vblank
@busy:
    lda $4212
    and #$0001
    bne @busy
    lda $4218
    and #$0080
    beq @up
    lda $38
    bne @held
    inc $36
    lda #$0001
    sta $38
    sep #$20
    lda #$01
    sta $4016
    stz $4016
    rep #$20
    stz $3A
    ldx #$0010
@ser:
    lda $4016
    lsr a
    rol $3A
    dex
    bne @ser
    lda $3A
    cmp $4218
    bne @bad
    bra @held
@up:
    stz $38
    lda $36
    cmp #$0002
    bcs @enough
@held:
    dey
    bne @frame
@bad:
    sep #$20
    stz $4200
    bra @fail
@enough:
    sep #$20
    stz $4200
"""),
    ([87], "Write-twice registers: M7A byte order decides the PPU multiplier result", """
    lda #$34
    sta $211B
    lda #$12
    sta $211B
    lda #$56
    sta $211C
    lda $2134
    cmp #${p1_0:02X}
    bne @fail
    lda $2135
    cmp #${p1_1:02X}
    bne @fail
    lda $2136
    cmp #${p1_2:02X}
    bne @fail
    lda #$12
    sta $211B
    lda #$34
    sta $211B
    lda #$56
    sta $211C
    lda $2134
    cmp #${p2_0:02X}
    bne @fail
    lda $2136
    cmp #${p2_2:02X}
    bne @fail
    lda #$DC
    sta $211B
    lda #$FE
    sta $211B
    lda #$9A
    sta $211C
    lda $2134
    cmp #${p3_0:02X}
    bne @fail
    lda $2135
    cmp #${p3_1:02X}
    bne @fail
    lda $2136
    cmp #${p3_2:02X}
    bne @fail
""".replace("{p1_0:02X}", f"{M7_1 & 0xFF:02X}").replace("{p1_1:02X}", f"{(M7_1 >> 8) & 0xFF:02X}")
       .replace("{p1_2:02X}", f"{M7_1 >> 16:02X}").replace("{p2_0:02X}", f"{M7_2 & 0xFF:02X}")
       .replace("{p2_2:02X}", f"{M7_2 >> 16:02X}").replace("{p3_0:02X}", f"{M7_3 & 0xFF:02X}")
       .replace("{p3_1:02X}", f"{(M7_3 >> 8) & 0xFF:02X}").replace("{p3_2:02X}", f"{M7_3 >> 16:02X}")),
    ([88], "A V-timer IRQ fires on the programmed line (latched V counter)", r"""
    lda $4211
    ldx #$0064
    stx $4209
    stz $0086
    stz $0087
    lda #$20
    sta $4200
    cli
    ldy #$0000
@wait:
    dey
    beq @timeout
    lda $0086
    beq @wait
    sei
    stz $4200
    ldx $0088
    cpx #$0064
    bne @fail
    bra @done
@timeout:
    sei
    stz $4200
    bra @fail
@done:
"""),
    ([71], "Immediate operands are taken as encoded (#10 is decimal ten, #$10 sixteen)", r"""
    lda #10
    cmp #$0A
    bne @fail
    lda #$10
    cmp #16
    bne @fail
"""),
    ([76], "Parameters inline after JSR: the callee reads them and moves its return address", r"""
    stz $3C
    jsr add_inline
    .db $05
    jsr add_inline
    .db $0B
    lda $3C
    cmp #$10
    bne @fail
"""),
    ([39], "(dp,X) in emulation mode with DL != 0: the pointer's high byte wraps within the page", r"""
    lda #$34
    sta $02FF
    lda #$12
    sta $0200
    lda #$99
    sta $0300
    lda #$ED
    sta.l $7E1234
    lda #$7E
    pha
    plb
    rep #$20
    lda #$011A
    tcd
    sep #$20
    tsx
    stx.w $0070
    sec
    xce
.i8
    ldx #$EE
    lda ($F7,x)
    clc
    xce
    rep #$10
    ldx.w $0070
    txs
    sta.l $7E0072
    rep #$20
    lda #$0000
    tcd
    sep #$20
    lda #$00
    pha
    plb
    lda $72
    cmp #$ED
    bne @fail
"""),
]


FRAMEWORK = r"""
; Checklist test ROM: framework. Generated by make_checklist_rom.py.
test_index = $0080
saved_s = $0082
nmi_count = $0084
irq_count = $0086
irq_line = $0088
nmi_hvbjoy = $008E

.org $008000
reset:
    sei
    clc
    xce
    rep #$38
    ldx #$1FFF
    txs
    lda #$0000
    tcd
    sep #$20
    lda #$00
    pha
    plb
    lda #$80
    sta $2100
    stz $4200
    stz $420C
    stz $420D
    lda #$FF
    sta $4201
    rep #$20
    lda #$0000
    sta nmi_count
    sta irq_count
    ldx #$0000
clear_results:
    lda #$0000
    sta.l $7E2000,x
    inx
    inx
    cpx #$0200
    bne clear_results
    sep #$20
    ldx #$0000
    stx test_index
next_test:
    rep #$30
    tsx
    stx saved_s
    lda test_index
    asl a
    tax
    lda test_table,x
    beq all_done
    sep #$20
    jsr (test_table,x)
    sta.l $7E00F0
    clc
    xce
    rep #$38
    ldx saved_s
    txs
    lda #$0000
    tcd
    sep #$20
    lda #$00
    pha
    plb
    sei
    stz $4200
    stz $420C
    lda #$80
    sta $2100
    ldx test_index
    lda.l $7E00F0
    sta.l $7E2000,x
    inx
    stx test_index
    bra next_test
all_done:
    sep #$20
    lda test_index
    sta.l $7E2101
    lda #$D0
    sta.l $7E2100
forever:
    bra forever

; ---- interrupt handlers -------------------------------------------------
nmi_handler:
    rep #$30
    pha
    sep #$20
    lda $4212
    sta nmi_hvbjoy
    lda $4210
    rep #$20
    inc nmi_count
    pla
    rti

irq_handler:
    rep #$30
    pha
    sep #$20
    lda $4211
    lda $2137
    lda $213D
    sta irq_line
    lda $213D
    and #$01
    sta irq_line+1
    lda $213F
    rep #$20
    inc irq_count
    pla
    rti

cop_handler:
    sep #$30
    lda #$01
    sta $8A
    rep #$20
    lda $02,s
    dec a
    sta $8C
    sep #$20
    lda ($8C)
    sta $8B
    rti

brk_handler:
emu_handler:
    rti

; ---- helpers --------------------------------------------------------------
; Called with A 8-bit and X/Y 16-bit, like the tests.
.a8
.i16
wait_vblank:
    php
    sep #$20
wait_vblank_loop:
    lda $4212
    bpl wait_vblank_loop
    plp
    rts

wait_vblank_end:
    php
    sep #$20
wait_vblank_end_loop:
    lda $4212
    bmi wait_vblank_end_loop
    plp
    rts

near_depth:
    tsx
    stx $04
    rep #$20
    lda $00
    sec
    sbc #$0002
    sta $02
    sep #$20
    rts

is_above5:
    cmp #$06
    rts

clobber_keep_x:
    phx
    ldx #$0000
    ldy #$0099
    plx
    rts

add_inline:
    rep #$20
    pla
    inc a
    sta $3E
    pha
    sep #$20
    lda ($3E)
    clc
    adc $3C
    sta $3C
    rts

count_slow:
    rep #$20
    lda.w nmi_count
count_slow_sync:
    cmp.w nmi_count
    beq count_slow_sync
    lda.w nmi_count
    ldx #$0000
count_slow_loop:
    inx
    cmp.w nmi_count
    beq count_slow_loop
    sep #$20
    rts

count_wram_setup:
    ldx #$0000
count_wram_copy:
    lda wram_counter,x
    sta $1100,x
    inx
    cpx #wram_counter_end-wram_counter
    bne count_wram_copy
    rts

; Copied to $1100 and run from WRAM (same loop as count_slow).
wram_counter:
    .db $C2, $20            ; rep #$20
    .db $AD, $84, $00       ; lda nmi_count
    .db $CD, $84, $00       ; cmp nmi_count
    .db $F0, $FB            ; beq -5
    .db $AD, $84, $00       ; lda nmi_count
    .db $A2, $00, $00       ; ldx #$0000
    .db $E8                 ; inx
    .db $CD, $84, $00       ; cmp nmi_count
    .db $F0, $FA            ; beq -6
    .db $E2, $20            ; sep #$20
    .db $60                 ; rts
wram_counter_end:

dma_src:    .db $10, $32, $54, $76, $98, $BA, $DC, $FE
; VRAM reads come from a latch that reloads from the address before it
; increments: the first word after setting the address is read twice.
dma_readback: .db $10, $32, $10, $32, $54, $76, $98, $BA, $DC, $FE
dma_a:      .db $AA
dma_b:      .db $BB
dma_expect: .db $AA, $AA, $AA, $AA, $BB, $BB, $BB, $BB
hdma_table: .db $83, $11, $22, $33, $82, $44, $55, $00
hdma_expect: .db $11, $22, $33, $44, $55, $00
spc_prog:   .db $E4, $F4, $BC, $C4, $F5, $2F, $F9   ; loop: mov a,$F4 / inc a / mov $F5,a / bra loop

table256:
    .db $A0, $A1, $A2, $A3
    .fill 252, $77
"""

FAR_CODE = r"""
; ---- bank $01 --------------------------------------------------------------
.a8
.i16
.org $018000
far_phk:
    phk
    pla
    rtl

far_jmpind:
    jmp ($0050)
far_ret1:
    lda #$01
    rtl

far_jmpindx:
    ldx #$0002
    jmp (far_table,x)
far_ret2:
    lda #$02
    rtl
far_table:
    .dw far_ret1, far_ret2

far_ret3:
    lda #$03
    jml t_jml_back

far_depth:
    tsx
    stx $06
    rtl

.org $81C000
count_fast:
    lda #$01
    sta $420D
    rep #$20
    lda.w nmi_count
count_fast_sync:
    cmp.w nmi_count
    beq count_fast_sync
    lda.w nmi_count
    ldx #$0000
count_fast_loop:
    inx
    cmp.w nmi_count
    beq count_fast_loop
    sep #$20
    stz $420D
    rtl

.org $01FFFE
    .db $80, $40            ; bra: wraps from $01:0000 to $01:0040 (WRAM mirror)
"""

HEADER = r"""
.org $00FFC0
    .db "NATIVE CONSOLE CHECK "
    .db $30, $00, $07, $00, $01, $00, $00
    .dw $0000, $FFFF
    .dw 0, 0
    .dw cop_handler, brk_handler, 0, nmi_handler, 0, irq_handler
    .dw 0, 0
    .dw emu_handler, 0, 0, emu_handler, reset, emu_handler
"""


def build_source(only: set[int] | None = None) -> tuple[str, list[dict]]:
    out = [FRAMEWORK]
    meta = []
    table = ["test_table:"]
    for index, (nuances, text, body) in enumerate(TESTS):
        if only is not None and index not in only:
            continue
        label = f"t{index:02d}"
        # Failure exits may be far away: use the long forms (inverse branch
        # over a JMP). The branch being tested still decides the outcome.
        for b in ("bne", "beq", "bcc", "bcs", "bmi", "bpl", "bvc", "bvs"):
            body = body.replace(f"    {b} @fail", f"    j{b[1:]} @fail")
        body = body.replace("bra @fail", "jmp @fail")
        body = body.replace("@fail", f"{label}_fail").replace("@", f"{label}_")
        if 39 in nuances and "JML" in text:
            body = body.replace(f"{label}_back:", f"{label}_back:\nt_jml_back:")
        out.append(f"\n; test {index}: {text} (checklist {', '.join(map(str, nuances))})")
        out.append(".a8\n.i16")
        out.append(f"{label}:")
        out.append(body)
        out.append(f"    sep #$20\n    lda #$01\n    rts\n{label}_fail:\n    sep #$20\n    lda #$FF\n    rts")
        table.append(f"    .dw {label}")
        meta.append({"index": len(meta), "test": index, "nuances": nuances, "description": text})
    table.append("    .dw $0000")
    out.append("\n".join(table))
    if not any("t_jml_back:" in part for part in out):
        out.append("t_jml_back = forever  ; the JML test is not in this build")
    out.append(FAR_CODE)
    out.append(HEADER)
    return "\n".join(out) + "\n", meta


def fix_checksum(image: bytearray) -> None:
    image[0x7FDC:0x7FE0] = b"\xFF\xFF\x00\x00"
    total = sum(image) & 0xFFFF
    image[0x7FDE] = total & 0xFF
    image[0x7FDF] = total >> 8
    complement = total ^ 0xFFFF
    image[0x7FDC] = complement & 0xFF
    image[0x7FDD] = complement >> 8


def build(output: Path, json_path: Path | None, asm_path: Path | None, only: set[int] | None = None) -> None:
    source, meta = build_source(only)
    if asm_path:
        asm_path.write_text(source)
    asm = Assembler(ROM_SIZE)
    lines = source.splitlines()
    asm.assemble(lines, final=False)
    asm.assemble(lines, final=True)
    image = bytearray(asm.image)
    fix_checksum(image)
    output.write_bytes(bytes(image))
    if json_path:
        json_path.write_text(json.dumps({"tests": meta}, indent=2) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--asm", type=Path, help="also write the generated assembly source")
    parser.add_argument("--only", help="comma-separated test numbers (for debugging)")
    args = parser.parse_args()
    only = {int(v) for v in args.only.split(",")} if args.only else None
    try:
        build(args.output, args.json, args.asm, only)
    except AsmError as error:
        print(f"assembly failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
