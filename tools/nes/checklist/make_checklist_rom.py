#!/usr/bin/env python3
"""Build the NES recompiler checklist ROM.

An MMC1 cartridge (iNES mapper 1: four 16 KiB PRG banks, CHR RAM, 8 KiB PRG
RAM) of numbered tests, each a short 6502 routine whose result is checked in
the ROM. The tests are the 6502 and NES behaviour a static recompiler gets
wrong first: flags as values, computed jumps, code that moves under the
program counter when the cartridge switches banks, code in RAM, interrupts
that must arrive between two particular instructions. The same ROM is run by
the interpreter (nes_run), recompiled by nesrecomp, and run again natively
and in --differential lockstep (tools/nes/run_checklist.py).

Results:

    $0300 + n      test n (from 1): $01 pass, $FF fail, $00 not reached
    $02FE          the number of tests, once all have run
    $6000          blargg's protocol: $80 while running, then $00 if every
                   test passed, else the number of the first that did not
    $6001-$6003    DE B0 61
    $6004...       "ALL PASS" or "FAIL 3 17 22", zero-terminated
    the screen     green when everything passed, red otherwise

The ROM then idles in JMP *. Test 1 reads the controller, so the runner must
hold these buttons (KEYS below; also in the JSON):

    --keys 0:A+SELECT+UP+LEFT:90 --keys 90:B+START+DOWN+RIGHT:100000

Layout. Recompiled code is keyed by (8 KiB PRG page, PC), so the tests are
spread over all eight pages: banks 0-2 are switched in at $8000 (two pages
each), bank 3 is fixed at $C000 and holds reset, the interrupt handlers, the
driver and the bank-switch routines. Routines that must sit at an exact
address (the same address in several banks, or across a page boundary) are
placed with .org; the build fails if they move. Every bank ends with the
same reset stub, so the cartridge starts whichever bank the MMC1 powers up
with.

Expected values come from hardware documentation (the NESdev wiki: CPU,
"CPU interrupts", "CPU unofficial opcodes", "PPU registers", "APU Frame
Counter", "MMC1", "Standard controller", "Open bus behavior"), never from
running an emulator. The timing tests check a relation (iterations of a
fixed loop between two NMIs), not a cycle count.

Tests (number, where the test's code is: bank and L = first / H = second
8 KiB page, F = the fixed bank, what it checks):

     1  FL  Controller: strobe, eight reads give A B Select Start Up Down Left Right; scripted input
     2  FH  Controller: while the strobe is high every read returns the A button
     3  0L  ADC/SBC ignore the D flag (the 2A03 has no decimal mode); the flag itself exists
     4  1L  Carry is an input and an output of ADC/SBC; 16-bit add and subtract chain it
     5  2L  Overflow (V) and carry (C) differ; CLV
     6  0H  N and Z follow loads, transfers, INX/DEY and PLA; stores and TXS change no flag
     7  1H  CMP/CPX/CPY: unsigned outcome in C and Z; signed needs SBC and N xor V; CMP leaves V
     8  2H  BIT copies bits 7 and 6 of memory to N and V, Z from A AND memory; A unchanged
     9  0L  Shifts and rotates go through carry, in A and in memory; multi-byte shifts; a bit stream
    10  1L  INC/DEC/INX/DEX/INY/DEY leave C alone; 8-bit wraparound; a multi-byte counter
    11  FH  JMP ($xxFF) takes its high byte from $xx00 (the page-wrap bug)
    12  2L  RTS as a computed jump: push address-1 from a table, RTS
    13  0H  JSR with inline parameters: the callee reads the bytes after the call and returns past them
    14  1H  JSR pushes the address of its last byte, high byte first; RTS adds one
    15  0L  BRK pushes PC+2 and P with B set, sets I, uses the IRQ vector; the handler sees B; RTI returns
    16  2H  PHP pushes bits 4 and 5 set; PLP ignores them; RTI restores the flags and does not add one
    17  1L  The stack wraps inside page 1: pushes at S=$00 and S=$FF, pulls back, JSR/RTS across the wrap
    18  2L  Zero-page indexed addressing wraps inside page 0 (zp,X and zp,Y); absolute,X does not
    19  0H  (zp,X) wraps in page 0: the pointer at $FF takes its high byte from $00
    20  1H  (zp),Y carries into the high byte; a pointer at $FF wraps; abs,X and abs,Y cross pages
    21  2L  Loops: backward branches on Z, C and N, nested and counted
    22  2H  Branches across a 256-byte page boundary, forward and backward
    23  1L  Branches into the next and the previous 8 KiB page ($9FFF/$A000) and fall-through across it
    24  FL  An instruction that straddles $DFFF/$E000 (opcode at $DFFF) in the fixed bank; branches across it
    25  2H  A two-byte instruction that straddles $9FFF/$A000: the operand opens the next 8 KiB page
    26  0L  A JSR that straddles $9FFE-$A000: the pushed return address is in the next 8 KiB page
    27  FH  The BIT-skip trick: entry points in the middle of an instruction (one byte sequence, two decodings)
    28  1H  Results in flags (C, Z, V) survive RTS; a routine that saves X returns Y
    29  FL  Code copied to RAM ($0200) and called: jumps inside RAM, a call back into ROM
    30  FH  The same RAM code after its bytes change: operands, an opcode, and the next instruction to run
    31  0H  Code copied to PRG RAM ($6100), run, and run again after its bytes change
    32  FL  MMC1: $8000 holds a different routine in banks 0, 1 and 2; bank 3 appears at $8000 too; numbers wrap
    33  FL  MMC1: a switchable bank calls a fixed-bank routine that switches banks and returns into the new bank
    34  FH  MMC1: a routine switches its own bank out: the instruction after the fifth write is the new bank's
    35  FL  MMC1 PRG mode 2 (first bank fixed at $8000, the register picks $C000), run from RAM
    36  0L  MMC1 PRG mode 2 from the bank that stays at $8000: it runs on while $C000 and the vectors change
    37  FH  MMC1 PRG modes 0 and 1 (32 KiB; the low bank bit is ignored), run from RAM
    38  FL  MMC1 ignores the second of two writes on consecutive cycles (INC on a ROM byte holding $FF)
    39  FL  MMC1: a reset bit in the second of two consecutive writes still resets (INC on a byte holding $7F)
    40  FH  MMC1: a write with bit 7 set returns to "last bank fixed at $C000" and keeps the PRG register
    41  FL  JMP ($0010) through a RAM vector: one vector in two banks, a bank's other page, the fixed bank, RAM
    42  1L  NMI at vertical blank, once per frame; a wait-on-flag loop ends; no NMI while it is off
    43  2L  Polling $2002 bit 7 ends; the read clears the flag
    44  0H  APU frame IRQ: flag in $4015, cleared by reading; after CLI one more instruction runs first
    45  1H  SEI with an IRQ pending: the IRQ is taken once, after SEI, with I set in the pushed flags
    46  0L  $2007 reads lag one byte behind; the address steps by 1 or by 32
    47  1L  Palette reads are not delayed and fill the read buffer from the nametable underneath; $3F10 is $3F00
    48  2H  OAM DMA ($4014) copies 256 bytes; read back through $2004
    49  0L  RAM mirrors ($0800, $1000, $1800) and PPU register mirrors ($2008-$3FFF)
    50  FH  Open bus: $4018 and $5000 read as the last byte on the bus (the address's high byte)
    51  1H  Undocumented opcodes games use: LAX, SAX, DCP, ISC, SLO
    52  2L  Undocumented NOPs with operands are two and three bytes long and change nothing
    53  FL  Timing: iterations of an 11-cycle loop between two NMIs fit 29,780.67 CPU cycles per frame
    54  FH  Timing: the same loop run from RAM counts the same, give or take two

    python tools/nes/checklist/make_checklist_rom.py -o checklist.nes [--json tests.json] [--asm FILE] [--only N,N]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from asm6502 import AsmError, Assembler, write_ines  # noqa: E402

BANKS = 4  # 64 KiB PRG ROM
MAPPER = 1
RESULTS = 0x0300
# The input the runner must script: test 1 waits for the first pattern, then
# for the second.
KEYS = ["--keys", "0:A+SELECT+UP+LEFT:90", "--keys", "90:B+START+DOWN+RIGHT:100000"]
FRAMES = 1200  # frame limit for the runner; the ROM finishes in about 115


def serial(register: int, lead: str = "") -> str:
    """Five writes of A's low bits, lowest first, to an MMC1 register."""
    lines = [lead] if lead else []
    for i in range(5):
        if i:
            lines.append("    lsr a")
        lines.append(f"    sta ${register:04X}")
    return "\n".join(lines)


# (slot, description, body). The body is entered with JSR: I set, D clear,
# NMI and rendering off, the frame interrupt inhibited, PRG mode 3 with the
# test's own bank (bank 0 for a fixed-bank test) at $8000. It falls off its
# end to pass and jumps to @fail otherwise; '@' becomes the test's label.
# $00-$1F are scratch. A test that moves S puts it back.
TESTS: list[tuple[str, str, str]] = [
    ("FL", "Controller: strobe, eight reads give A B Select Start Up Down Left Right; scripted input", r"""
    lda #180
    sta frames
@first:
    jsr read_pad
    cmp #$AA            ; A, Select, Up, Left
    beq @got_first
    jsr wait_vbl
    dec frames
    bne @first
    jmp @fail
@got_first:
    lda #240
    sta frames
@second:
    jsr read_pad
    cmp #$55            ; B, Start, Down, Right
    beq @got_second
    jsr wait_vbl
    dec frames
    bne @second
    jmp @fail
@got_second:
"""),
    ("FH", "Controller: while the strobe is high every read returns the A button", r"""
    jsr read_pad        ; the second pattern is held from here on: A up, B down
    cmp #$55
    bne @fail
    lda #$01
    sta $4016           ; the shift register keeps reloading: no read moves on to B
    lda $4016
    and #$01
    bne @fail
    lda $4016
    and #$01
    bne @fail
    lda $4016
    and #$01
    bne @fail
    lda #$00
    sta $4016
    lda $4016           ; latched when the strobe fell: A, then B
    and #$01
    bne @fail
    lda $4016
    and #$01
    beq @fail
"""),
    ("0L", "ADC/SBC ignore the D flag (the 2A03 has no decimal mode); the flag itself exists", r"""
    sed
    clc
    lda #$09
    adc #$01
    cld
    cmp #$0A            ; a 6502 with decimal mode gives $10
    bne @fail
    sed
    sec
    lda #$10
    sbc #$01
    cld
    cmp #$0F
    bne @fail
    sed
    clc
    lda #$99
    adc #$01
    cld
    bcs @fail
    cmp #$9A
    bne @fail
    sed
    clc
    lda #$58
    adc #$46            ; binary: $9E, N and V set
    cld
    bvc @fail
    bpl @fail
    cmp #$9E
    bne @fail
    sed
    php
    cld
    pla
    and #$08
    beq @fail
"""),
    ("1L", "Carry is an input and an output of ADC/SBC; 16-bit add and subtract chain it", r"""
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
    clc                 ; $01FF + $0001
    lda #$FF
    adc #$01
    sta $00
    lda #$01
    adc #$00
    sta $01
    lda $00
    bne @fail
    lda $01
    cmp #$02
    bne @fail
    sec                 ; $0200 - $0001
    lda #$00
    sbc #$01
    sta $00
    lda #$02
    sbc #$00
    sta $01
    lda $00
    cmp #$FF
    bne @fail
    lda $01
    cmp #$01
    bne @fail
    clc
    lda #$F0
    adc #$20
    bcc @fail
    sec
    lda #$20
    sbc #$30
    bcs @fail
"""),
    ("2L", "Overflow (V) and carry (C) differ; CLV", r"""
    clc
    lda #$7F
    adc #$01
    bvc @fail
    bcs @fail
    bpl @fail
    clc
    lda #$FF
    adc #$01
    bvs @fail
    bcc @fail
    bne @fail
    clc
    lda #$80
    adc #$80
    bvc @fail
    bcc @fail
    bne @fail
    sec
    lda #$80
    sbc #$01
    bvc @fail
    bcc @fail
    cmp #$7F
    bne @fail
    sec
    lda #$00
    sbc #$01
    bvs @fail
    bcs @fail
    bpl @fail
    clc
    lda #$7F
    adc #$01
    clv
    bvs @fail
"""),
    ("0H", "N and Z follow loads, transfers, INX/DEY and PLA; stores and TXS change no flag", r"""
    lda #$80
    bpl @fail
    beq @fail
    lda #$00
    bmi @fail
    bne @fail
    ldx #$FF
    bpl @fail
    inx
    bne @fail
    ldy #$00
    dey
    bpl @fail
    cpy #$FF
    bne @fail
    lda #$00
    ldx #$80
    stx $00             ; a store changes no flag
    bpl @fail
    beq @fail
    sta $01
    beq @fail
    lda #$00
    pha
    lda #$55
    pla                 ; PLA sets Z
    bne @fail
    tsx
    stx $00
    lda #$00
    txs                 ; S keeps its value; X is not zero, Z stays set
    bne @fail
    lda #$01
    tsx                 ; TSX sets N: S is above $80 here
    bpl @fail
    cpx $00
    bne @fail
    lda #$80
    ldx #$01
    tax
    bpl @fail
    ldy #$01
    lda #$00
    tay
    bne @fail
"""),
    ("1H", "CMP/CPX/CPY: unsigned outcome in C and Z; signed needs SBC and N xor V; CMP leaves V", r"""
    lda #$40
    cmp #$41
    bcs @fail
    beq @fail
    bpl @fail
    cmp #$40
    bne @fail
    bcc @fail
    cmp #$3F
    bcc @fail
    beq @fail
    bmi @fail
    lda #$7F
    cmp #$80            ; unsigned: $7F is below $80
    bcs @fail
    bpl @fail           ; N says "less" though 127 > -128: N alone is not a signed compare
    lda #$40
    sta $00
    bit $00             ; V set
    lda #$7F
    cmp #$80
    bvc @fail           ; CMP did not touch V
    clv
    cmp #$80
    bvs @fail
    sec
    lda #$7F
    sbc #$80            ; $FF with V: N xor V = 0, greater or equal
    bvc @fail
    bpl @fail
    sec
    lda #$80
    sbc #$7F            ; $01 with V: N xor V = 1, less
    bvc @fail
    bmi @fail
    ldx #$10
    cpx #$10
    bne @fail
    bcc @fail
    ldy #$10
    cpy #$11
    bcs @fail
    lda #$11
    sta $01
    cpx $01
    bcs @fail
    cpy $01
    beq @fail
"""),
    ("2H", "BIT copies bits 7 and 6 of memory to N and V, Z from A AND memory; A unchanged", r"""
    lda #$C0
    sta $00
    lda #$01
    bit $00
    bpl @fail
    bvc @fail
    bne @fail
    cmp #$01
    bne @fail
    lda #$3F
    sta $0400
    lda #$FF
    bit $0400
    bmi @fail
    bvs @fail
    beq @fail
    lda #$80
    sta $00
    bit $00
    bpl @fail
    bvs @fail
    beq @fail
"""),
    ("0L", "Shifts and rotates go through carry, in A and in memory; multi-byte shifts; a bit stream", r"""
    sec
    lda #$80
    rol a
    bcc @fail
    cmp #$01
    bne @fail
    sec
    lda #$01
    ror a
    bcc @fail
    cmp #$80
    bne @fail
    clc
    lda #$01
    ror a
    bcc @fail
    bne @fail
    lda #$81
    asl a
    bcc @fail
    cmp #$02
    bne @fail
    lda #$01
    lsr a
    bcc @fail
    bne @fail
    lda #$55
    sta $00
    sec
    rol $00
    bcs @fail
    lda $00
    cmp #$AB
    bne @fail
    clc
    ror $00
    bcc @fail
    lda $00
    cmp #$55
    bne @fail
    lda #$40            ; $8040 << 1 = $0080, carry out
    sta $00
    lda #$80
    sta $01
    asl $00
    rol $01
    bcc @fail
    lda $00
    cmp #$80
    bne @fail
    lda $01
    bne @fail
    lda #$80            ; $0180 >> 1 = $00C0
    sta $00
    lda #$01
    sta $01
    lsr $01
    ror $00
    bcs @fail
    lda $00
    cmp #$C0
    bne @fail
    lda #$B1            ; reverse the bits of $B1
    sta $00
    lda #$00
    ldx #$08
@reverse:
    lsr $00
    rol a
    dex
    bne @reverse
    cmp #$8D
    bne @fail
    lda #$40
    sta $0410
    ldx #$10
    asl $0400,x
    asl $0400,x
    bcc @fail
    lda $0410
    bne @fail
"""),
    ("1L", "INC/DEC/INX/DEX/INY/DEY leave C alone; 8-bit wraparound; a multi-byte counter", r"""
    sec
    ldx #$FF
    inx
    bne @fail
    bcc @fail
    clc
    dex
    bcs @fail
    cpx #$FF
    bne @fail
    lda #$FF
    sta $00
    sec
    inc $00
    bne @fail
    bcc @fail
    dec $00
    bpl @fail
    lda $00
    cmp #$FF
    bne @fail
    ldy #$00
    clc
    dey
    bcs @fail
    iny
    bne @fail
    lda #$FF
    clc
    adc #$01
    bne @fail
    bcc @fail
    lda #$FF            ; INC gives no carry: a counter chains on Z
    sta $00
    lda #$00
    sta $01
    inc $00
    bne @no_carry
    inc $01
@no_carry:
    lda $01
    cmp #$01
    bne @fail
"""),
    ("FH", "JMP ($xxFF) takes its high byte from $xx00 (the page-wrap bug)", r"""
    lda #lo(jmpbug_good)
    sta $04FF
    lda #hi(jmpbug_good)
    sta $0400           ; the byte the 6502 uses
    lda #hi(jmpbug_bad)
    sta $0500           ; the byte a carry into the high address byte would reach
    jsr jmpbug_go
    cmp #$5A
    bne @fail
"""),
    ("2L", "RTS as a computed jump: push address-1 from a table, RTS", r"""
    lda #$00
    sta $00
    ldx #$00
    jsr @dispatch
    ldx #$04
    jsr @dispatch
    lda $00
    cmp #$05
    bne @fail
    ldx #$02
    jsr @dispatch
    lda $00
    cmp #$07
    bne @fail
    jmp @done
@dispatch:
    lda @table+1,x
    pha
    lda @table,x
    pha
    rts
@h0:
    lda #$01
    ora $00
    sta $00
    rts
@h1:
    lda #$02
    ora $00
    sta $00
    rts
@h2:
    lda #$04
    ora $00
    sta $00
    rts
@table:
    .dw @h0-1, @h1-1, @h2-1
@done:
"""),
    ("0H", "JSR with inline parameters: the callee reads the bytes after the call and returns past them", r"""
    lda #$00
    sta $02
    jsr add_inline
    .db $12, $34
    jsr add_inline
    .db $01, $02
    lda $02
    cmp #$49
    bne @fail
"""),
    ("1H", "JSR pushes the address of its last byte, high byte first; RTS adds one", r"""
    tsx
    stx $04
    jsr @sub
@ret:
    tsx
    cpx $04
    bne @fail
    lda $00
    cmp #lo(@ret-1)
    bne @fail
    lda $01
    cmp #hi(@ret-1)
    bne @fail
    lda $02
    cmp #$02
    bne @fail
    jmp @done
@sub:
    tsx
    lda $0101,x         ; pushed last: the low byte
    sta $00
    lda $0102,x
    sta $01
    stx $03
    sec
    lda $04
    sbc $03
    sta $02
    rts
@done:
"""),
    ("0L", "BRK pushes PC+2 and P with B set, sets I, uses the IRQ vector; the handler sees B; RTI returns", r"""
    lda #$00
    sta brk_count
    sta irq_count
    pha
    plp                 ; every flag clear, I too (no IRQ source is active)
    sec
    brk
    .db $EA             ; the byte BRK skips
@after:
    php
    sei
    pla
    cmp #$31            ; RTI brought back C set and I clear
    bne @fail
    lda brk_count
    cmp #$01
    bne @fail
    lda irq_count
    bne @fail
    lda brk_p
    cmp #$31            ; pushed with B and bit 5 set, before I was set
    bne @fail
    lda brk_pcl
    cmp #lo(@after)
    bne @fail
    lda brk_pch
    cmp #hi(@after)
    bne @fail
    lda brk_in_p
    and #$04            ; I was set inside the handler
    beq @fail
"""),
    ("2H", "PHP pushes bits 4 and 5 set; PLP ignores them; RTI restores the flags and does not add one", r"""
    lda #$00
    pha
    plp
    php
    pla
    sei
    cmp #$30
    bne @fail
    lda #$FF
    pha
    plp
    php
    pla
    cld
    cmp #$FF
    bne @fail
    lda #$CF
    pha
    plp                 ; bits 4 and 5 of the pulled byte do not exist
    php
    pla
    cld
    cmp #$FF
    bne @fail
    lda #hi(@target)
    pha
    lda #lo(@target)
    pha
    lda #$C7            ; N V I Z C
    pha
    lda #$00
    clc
    rti                 ; to @target itself, not @target+1
    jmp @fail
@target:
    bpl @fail
    bvc @fail
    bne @fail
    bcc @fail
    php
    pla
    cmp #$F7
    bne @fail
    clv
"""),
    ("1L", "The stack wraps inside page 1: pushes at S=$00 and S=$FF, pulls back, JSR/RTS across the wrap", r"""
    lda $01FF           ; the driver's return address lives up here
    sta $06
    lda $0100
    sta $07
    tsx
    stx $00
    ldx #$00
    txs
    lda #$A1
    pha                 ; to $0100; S becomes $FF
    lda #$B2
    pha                 ; to $01FF
    tsx
    stx $01
    lda $0100
    sta $08
    lda $01FF
    sta $09
    pla
    sta $02
    pla                 ; S goes from $FF to $00
    sta $03
    tsx
    stx $04
    jsr @sub            ; S = $00: high byte to $0100, low byte to $01FF
@ret:
    tsx
    stx $05
    ldx $00
    txs
    lda $06
    sta $01FF
    lda $07
    sta $0100
    lda $01
    cmp #$FE
    bne @fail
    lda $08
    cmp #$A1
    bne @fail
    lda $09
    cmp #$B2
    bne @fail
    lda $02
    cmp #$B2
    bne @fail
    lda $03
    cmp #$A1
    bne @fail
    lda $04
    bne @fail
    lda $05
    bne @fail
    lda $0A
    cmp #hi(@ret-1)
    bne @fail
    lda $0B
    cmp #lo(@ret-1)
    bne @fail
    jmp @done
@sub:
    lda $0100
    sta $0A
    lda $01FF
    sta $0B
    rts
@done:
"""),
    ("2L", "Zero-page indexed addressing wraps inside page 0 (zp,X and zp,Y); absolute,X does not", r"""
    lda #$11
    sta $05
    lda #$22
    sta $0105
    ldx #$06
    lda $FF,x           ; $05, not $0105
    cmp #$11
    bne @fail
    lda #$33
    sta $FF,x
    lda $05
    cmp #$33
    bne @fail
    lda $0105
    cmp #$22
    bne @fail
    ldy #$07
    ldx $FE,y
    cpx #$33
    bne @fail
    ldx #$44
    stx $FE,y
    lda $05
    cmp #$44
    bne @fail
    ldx #$06
    inc $FF,x
    lda $05
    cmp #$45
    bne @fail
    lda.w $00FF,x       ; absolute: $0105
    cmp #$22
    bne @fail
"""),
    ("0H", "(zp,X) wraps in page 0: the pointer at $FF takes its high byte from $00", r"""
    lda #$34
    sta $FF
    lda #$04
    sta $00
    lda #$05
    sta $0100
    lda #$77
    sta $0434
    lda #$88
    sta $0534
    ldx #$01
    lda ($FE,x)         ; pointer bytes at $FF and $00
    cmp #$77
    bne @fail
    lda #$99
    sta ($FE,x)
    lda $0434
    cmp #$99
    bne @fail
    lda #$34            ; $80 + $90 wraps to $10
    sta $10
    lda #$05
    sta $11
    ldx #$90
    lda ($80,x)
    cmp #$88
    bne @fail
"""),
    ("1H", "(zp),Y carries into the high byte; a pointer at $FF wraps; abs,X and abs,Y cross pages", r"""
    lda #$F0
    sta $02
    lda #$04
    sta $03
    lda #$5A
    sta $0510
    lda #$A5
    sta $0410
    ldy #$20
    lda ($02),y         ; $04F0 + $20 = $0510
    cmp #$5A
    bne @fail
    lda #$6B
    sta ($02),y
    lda $0510
    cmp #$6B
    bne @fail
    lda $0410
    cmp #$A5
    bne @fail
    lda #$F0            ; a pointer at $FF: high byte from $00, not $0100
    sta $FF
    lda #$04
    sta $00
    lda #$06
    sta $0100
    lda #$EE
    sta $0710
    lda ($FF),y
    cmp #$6B
    bne @fail
    ldx #$20
    lda $04F0,x
    cmp #$6B
    bne @fail
    lda $04F0,y
    cmp #$6B
    bne @fail
    ldx #$FF
    lda $0411,x
    cmp #$6B
    bne @fail
"""),
    ("2L", "Loops: backward branches on Z, C and N, nested and counted", r"""
    lda #$00
    ldx #$0A
    clc
@sum:
    stx $00
    adc $00
    dex
    bne @sum
    cmp #$37            ; 1 + 2 + ... + 10
    bne @fail
    lda #$00
    sta $01
    ldy #$03
@outer:
    ldx #$05
@inner:
    inc $01
    dex
    bne @inner
    dey
    bne @outer
    lda $01
    cmp #$0F
    bne @fail
    ldx #$00
@up:
    inx
    cpx #$C8
    bcc @up
    cpx #$C8
    bne @fail
    lda #$01
    ldy #$00
@shift:
    iny
    asl a
    bcc @shift
    cpy #$08
    bne @fail
    ldx #$04
    lda #$00
    sta $02
@down:
    inc $02
    dex
    bpl @down
    lda $02
    cmp #$05
    bne @fail
"""),
    ("2H", "Branches across a 256-byte page boundary, forward and backward", r"""
    jsr pagex
    cmp #$03
    bne @fail
"""),
    ("1L", "Branches into the next and the previous 8 KiB page ($9FFF/$A000) and fall-through across it", r"""
    jsr b1_cross
    cmp #$07
    bne @fail
"""),
    ("FL", "An instruction that straddles $DFFF/$E000 (opcode at $DFFF) in the fixed bank; branches across it", r"""
    jsr fx_cross
    cmp #$31
    bne @fail
    lda $06
    cmp #$31
    bne @fail
    lda $05
    cmp #$02
    bne @fail
"""),
    ("2H", "A two-byte instruction that straddles $9FFF/$A000: the operand opens the next 8 KiB page", r"""
    jsr b2_straddle
    cmp #$C3
    bne @fail
    cpx #$01
    bne @fail
"""),
    ("0L", "A JSR that straddles $9FFE-$A000: the pushed return address is in the next 8 KiB page", r"""
    jsr b0_straddle
    cmp #$48
    bne @fail
    lda $00
    bne @fail           ; the pushed address is $A000
    lda $01
    cmp #$A0
    bne @fail
"""),
    ("FH", "The BIT-skip trick: entry points in the middle of an instruction (one byte sequence, two decodings)", r"""
    jsr @entry1
    cmp #$01
    bne @fail
    jsr @entry2
    cmp #$02
    bne @fail
    jsr @entry3
    cmp #$03
    bne @fail
    jmp @done
@entry1:
    lda #$01
    .db $2C             ; BIT abs: its operand is the next instruction
@entry2:
    lda #$02
    .db $2C
@entry3:
    lda #$03
    rts
@done:
"""),
    ("1H", "Results in flags (C, Z, V) survive RTS; a routine that saves X returns Y", r"""
    lda #$05
    jsr @at_least_6
    bcs @fail
    lda #$06
    jsr @at_least_6
    bcc @fail
    lda #$07
    jsr @is_zero
    beq @fail
    lda #$00
    jsr @is_zero
    bne @fail
    ldx #$42
    jsr @keep_x
    cpx #$42
    bne @fail
    cpy #$99
    bne @fail
    clv
    jsr @set_v
    bvc @fail
    jmp @done
@at_least_6:
    cmp #$06
    rts
@is_zero:
    cmp #$00
    rts
@keep_x:
    txa
    pha
    ldx #$00
    ldy #$99
    pla
    tax
    rts
@set_v:
    bit @v_byte
    rts
@v_byte:
    .db $40
@done:
"""),
    ("FL", "Code copied to RAM ($0200) and called: jumps inside RAM, a call back into ROM", r"""
    jsr copy_ram_add
    jsr ram_add
    cmp #$34            ; $11 + $22 in RAM, plus one in ROM
    bne @fail
    jsr ram_patch_next
    cmp #$01
    bne @fail
    jsr ram_self
    cmp #$00
    bne @fail
"""),
    ("FH", "The same RAM code after its bytes change: operands, an opcode, and the next instruction to run", r"""
    jsr copy_ram_add
    jsr ram_add
    cmp #$34
    bne @fail
    lda #$40
    sta ram_add+1       ; LDA's operand
    jsr ram_add
    cmp #$63
    bne @fail
    lda #$49            ; ADC # becomes EOR #
    sta ram_add+3
    lda #$0F
    sta ram_add+4
    jsr ram_add
    cmp #$50
    bne @fail
    jsr ram_self        ; rewrites its own operand: 0, 1, 2
    cmp #$00
    bne @fail
    jsr ram_self
    cmp #$01
    bne @fail
    jsr ram_self
    cmp #$02
    bne @fail
    jsr ram_patch_next  ; stores an INX over the NOP it is about to run
    cmp #$01
    bne @fail
"""),
    ("0H", "Code copied to PRG RAM ($6100), run, and run again after its bytes change", r"""
    jsr copy_pram
    jsr pram_code
    cmp #$C3
    bne @fail
    lda #$0C
    sta pram_data
    jsr pram_code
    cmp #$CC
    bne @fail
    lda #$2D            ; ORA abs becomes AND abs
    sta pram_code+2
    lda #$41
    sta pram_data
    jsr pram_code
    cmp #$40
    bne @fail
"""),
    ("FL", "MMC1: $8000 holds a different routine in banks 0, 1 and 2; bank 3 appears at $8000 too; numbers wrap", r"""
    lda #$00
    jsr set_prg
    jsr $8000
    cmp #$A0
    bne @fail
    lda #$01
    jsr set_prg
    jsr $8000
    cmp #$B1
    bne @fail
    lda #$02
    jsr set_prg
    jsr $8000
    cmp #$C2
    bne @fail
    lda #$03
    jsr set_prg         ; the fixed bank a second time, at $8000
    jsr $8000
    cmp #$D3
    bne @fail
    jsr bank_id
    cmp #$D3
    bne @fail
    lda #$05
    jsr set_prg         ; four banks: 5 is bank 1
    jsr $8000
    cmp #$B1
    bne @fail
    lda #$00
    jsr set_prg
    jsr $8000
    cmp #$A0
    bne @fail
"""),
    ("FL", "MMC1: a switchable bank calls a fixed-bank routine that switches banks and returns into the new bank", r"""
    lda #$00
    jsr set_prg
    jsr $8100           ; bank 0 calls set_prg(1); bank 1 goes on and calls set_prg(2); bank 2 returns
    cmp #$E2
    bne @fail
"""),
    ("FH", "MMC1: a routine switches its own bank out: the instruction after the fifth write is the new bank's", r"""
    lda #$00
    jsr set_prg
    jsr $8200           ; bank 0 selects bank 2, which selects bank 1, which returns
    cmp #$F1
    bne @fail
"""),
    ("FL", "MMC1 PRG mode 2 (first bank fixed at $8000, the register picks $C000), run from RAM", r"""
    jsr copy_ram_mmc
    jsr ram_mode2
    lda $00
    cmp #$B1            ; bank 1 at $C000
    bne @fail
    lda $01
    cmp #$A0            ; bank 0 at $8000
    bne @fail
    lda $02
    cmp #$C2
    bne @fail
    lda $03
    cmp #$A0
    bne @fail
    lda $04
    cmp #$D3
    bne @fail
    jsr $8000           ; mode 3 again
    cmp #$A0
    bne @fail
"""),
    ("0L", "MMC1 PRG mode 2 from the bank that stays at $8000: it runs on while $C000 and the vectors change", r"""
    lda #$0A
    jsr @ctrl           ; mode 2: this bank is fixed at $8000, the PRG register (0) picks $C000
    jsr $C000           ; this bank again
    sta $00
    lda #$01
    jsr @prg
    jsr $C000
    sta $01
    lda #$02
    jsr @prg
    jsr $C000
    sta $02
    lda #$00
    jsr @prg            ; so that mode 3 leaves this bank at $8000
    lda #$0E
    jsr @ctrl           ; mode 3: bank 3 is back at $C000
    jsr bank_id
    sta $03
    lda $00
    cmp #$A0
    bne @fail
    lda $01
    cmp #$B1
    bne @fail
    lda $02
    cmp #$C2
    bne @fail
    lda $03
    cmp #$D3
    bne @fail
    jmp @done
@ctrl:
@SERIAL_8000@
    rts
@prg:
@SERIAL_E000@
    rts
@done:
"""),
    ("FH", "MMC1 PRG modes 0 and 1 (32 KiB; the low bank bit is ignored), run from RAM", r"""
    jsr copy_ram_mmc
    jsr ram_mode0
    lda $00
    cmp #$A0
    bne @fail
    lda $01
    cmp #$B1
    bne @fail
    lda $02
    cmp #$A0
    bne @fail
    lda $03
    cmp #$B1
    bne @fail
    lda $04
    cmp #$C2
    bne @fail
    lda $05
    cmp #$D3
    bne @fail
    lda $06
    cmp #$C2
    bne @fail
    lda $07
    cmp #$D3
    bne @fail
"""),
    ("FL", "MMC1 ignores the second of two writes on consecutive cycles (INC on a ROM byte holding $FF)", r"""
    inc rom_ff          ; writes $FF (a reset), then $00 on the next cycle: ignored
    lda #$01
    jsr set_prg         ; were the $00 shifted in, these five writes would load 2
    jsr $8000
    cmp #$B1
    bne @fail
    lda #$01
    sta $E000
    sta $E000           ; two stray bits
    inc rom_ff          ; the reset drops them
    lda #$02
    jsr set_prg
    jsr $8000
    cmp #$C2
    bne @fail
"""),
    ("FL", "MMC1: a reset bit in the second of two consecutive writes still resets (INC on a byte holding $7F)", r"""
    inc rom_7f          ; writes $7F (a 1 bit), then $80: the reset is never ignored
    lda #$01
    jsr set_prg         ; were the 1 bit still there, these writes would load 3
    jsr $8000
    cmp #$B1
    bne @fail
"""),
    ("FH", "MMC1: a write with bit 7 set returns to \"last bank fixed at $C000\" and keeps the PRG register", r"""
    jsr copy_ram_mmc
    jsr ram_reset_mode
    lda $00
    cmp #$B1            ; mode 2: bank 1 at $C000
    bne @fail
    lda $01
    cmp #$A0
    bne @fail
    lda $02
    cmp #$D3            ; after the reset: bank 3 at $C000
    bne @fail
    lda $03
    cmp #$B1            ; and the PRG register's bank 1 at $8000
    bne @fail
"""),
    ("FL", "JMP ($0010) through a RAM vector: one vector in two banks, a bank's other page, the fixed bank, RAM", r"""
    lda #lo(ind_b0)
    sta $10
    lda #hi(ind_b0)
    sta $11
    lda #$00
    jsr set_prg
    jsr ind_go
    cmp #$70
    bne @fail
    lda #$01
    jsr set_prg
    jsr ind_go          ; the same vector, another bank
    cmp #$71
    bne @fail
    lda #lo(ind_b1_high)
    sta $10
    lda #hi(ind_b1_high)
    sta $11
    jsr ind_go
    cmp #$72
    bne @fail
    lda #lo(ind_fixed)
    sta $10
    lda #hi(ind_fixed)
    sta $11
    jsr ind_go
    cmp #$73
    bne @fail
    lda #$A9            ; LDA #$74 / RTS at $0200
    sta $0200
    lda #$74
    sta $0201
    lda #$60
    sta $0202
    lda #$00
    sta $10
    lda #$02
    sta $11
    jsr ind_go
    cmp #$74
    bne @fail
    lda #lo(ind_b0)
    sta $10
    lda #hi(ind_b0)
    sta $11
    lda #$00
    jsr set_prg
    jsr ind_go
    cmp #$70
    bne @fail
"""),
    ("1L", "NMI at vertical blank, once per frame; a wait-on-flag loop ends; no NMI while it is off", r"""
    lda #$00
    sta nmi_count
    jsr wait_vbl
    bcs @fail
    jsr wait_vbl
    bcs @fail
    lda nmi_count
    bne @fail           ; NMI is off
    bit $2002           ; drop the flag: turning NMI on with it set would fire at once
    lda #$80
    sta $2000
    jsr wait_nmi        ; with a time limit, so a machine without NMI fails instead of hanging
    bcs @off
    ldx #$03
@frame:
    lda nmi_count
@wait:
    cmp nmi_count       ; the handler changes it
    beq @wait
    dex
    bne @frame
    lda #$00
    sta $2000
    lda nmi_count
    cmp #$04            ; one per frame
    bne @fail
    jsr wait_vbl
    jsr wait_vbl
    lda nmi_count
    cmp #$04
    bne @fail
    jmp @done
@off:
    lda #$00
    sta $2000
    jmp @fail
@done:
"""),
    ("2L", "Polling $2002 bit 7 ends; the read clears the flag", r"""
    jsr wait_vbl        ; with a time limit
    bcs @fail
@poll:
    bit $2002
    bpl @poll
    lda $2002           ; the read above cleared it
    bmi @fail
    lda $2002
    bmi @fail
"""),
    ("0H", "APU frame IRQ: flag in $4015, cleared by reading; after CLI one more instruction runs first", r"""
    lda #$00
    sta irq_count
    sta brk_count
    sta $4017           ; four-step sequence, frame interrupt on
    ldx #$00
    ldy #$00
@poll:
    lda $4015
    and #$40
    bne @seen
    inx
    bne @poll
    iny
    cpy #$20            ; about three and a half frames
    bne @poll
    jmp @off
@seen:
    lda $4015           ; the flag is set on three cycles in a row: read once more
    lda $4015
    and #$40
    bne @off            ; reading cleared it
    lda irq_count
    bne @off            ; I is set: nothing was taken
    jsr delay_frame     ; the flag is set again and holds the IRQ line
    ldx #$00
    cli
    inx                 ; runs before the IRQ: CLI's effect comes one instruction late
@after:
    nop
    nop
    sei
    lda #$40
    sta $4017
    lda irq_count
    cmp #$01
    bne @fail
    lda irq_x
    cmp #$01
    bne @fail
    lda irq_pcl
    cmp #lo(@after)
    bne @fail
    lda irq_pch
    cmp #hi(@after)
    bne @fail
    lda irq_4015
    and #$40
    beq @fail
    lda irq_p
    and #$34
    cmp #$20            ; pushed with B clear and I clear
    bne @fail
    lda brk_count
    bne @fail
    lda $4015
    and #$40
    bne @fail
    jmp @done
@off:
    lda #$40
    sta $4017
    jmp @fail
@done:
"""),
    ("1H", "SEI with an IRQ pending: the IRQ is taken once, after SEI, with I set in the pushed flags", r"""
    lda #$00
    sta irq_count
    sta $4017
    jsr delay_frame     ; the frame interrupt is waiting, I is set
    cli                 ; not taken after CLI
    sei                 ; seen before SEI set I: taken after SEI
@after:
    nop
    lda #$40
    sta $4017
    lda irq_count
    cmp #$01
    bne @fail
    lda irq_pcl
    cmp #lo(@after)
    bne @fail
    lda irq_pch
    cmp #hi(@after)
    bne @fail
    lda irq_p
    and #$04
    beq @fail
"""),
    ("0L", "$2007 reads lag one byte behind; the address steps by 1 or by 32", r"""
    bit $2002
    lda #$00
    sta $2000           ; step 1
    lda #$21
    sta $2006
    lda #$00
    sta $2006
    lda #$11
    sta $2007           ; $2100-$2103
    lda #$22
    sta $2007
    lda #$33
    sta $2007
    lda #$66
    sta $2007
    lda #$04
    sta $2000           ; step 32
    lda #$21
    sta $2006
    lda #$20
    sta $2006
    lda #$44
    sta $2007           ; $2120
    lda #$55
    sta $2007           ; $2140
    lda #$00
    sta $2000
    lda #$21
    sta $2006
    lda #$00
    sta $2006
    lda $2007           ; whatever the buffer held
    lda $2007
    cmp #$11
    bne @fail
    lda $2007
    cmp #$22
    bne @fail
    lda $2007
    cmp #$33
    bne @fail           ; the buffer now holds $2103's byte
    lda #$21
    sta $2006
    lda #$00
    sta $2006
    lda $2007
    cmp #$66            ; the old buffer, not $2100's byte
    bne @fail
    lda #$04
    sta $2000
    lda #$21
    sta $2006
    lda #$00
    sta $2006
    lda $2007
    lda $2007
    cmp #$11
    bne @fail
    lda $2007
    cmp #$44
    bne @fail
    lda $2007
    cmp #$55
    bne @fail
    lda #$00
    sta $2000
"""),
    ("1L", "Palette reads are not delayed and fill the read buffer from the nametable underneath; $3F10 is $3F00", r"""
    bit $2002
    lda #$00
    sta $2000
    lda #$2F
    sta $2006
    lda #$01
    sta $2006
    lda #$9C
    sta $2007           ; $2F01 and $2F02: the nametable bytes "under" $3F01 and $3F02
    lda #$9D
    sta $2007
    lda #$3F
    sta $2006
    lda #$01
    sta $2006
    lda #$16
    sta $2007
    lda #$2A
    sta $2007
    lda #$3F
    sta $2006
    lda #$01
    sta $2006
    lda $2007           ; at once
    and #$3F
    cmp #$16
    bne @fail
    lda $2007
    and #$3F
    cmp #$2A
    bne @fail
    lda #$21
    sta $2006
    lda #$00
    sta $2006
    lda $2007           ; the buffer: filled from $2F02 by the last palette read
    cmp #$9D
    bne @fail
    lda #$3F
    sta $2006
    lda #$00
    sta $2006
    lda #$21
    sta $2007
    lda #$3F
    sta $2006
    lda #$10
    sta $2006
    lda #$0F
    sta $2007           ; $3F10 is the same cell as $3F00
    lda #$3F
    sta $2006
    lda #$00
    sta $2006
    lda $2007
    and #$3F
    cmp #$0F
    bne @fail
    lda #$00
    sta $2006
    sta $2006
"""),
    ("2H", "OAM DMA ($4014) copies 256 bytes; read back through $2004", r"""
    ldx #$00
@fill:
    txa
    eor #$5A
    sta $0700,x
    txa
    and #$03
    cmp #$02
    bne @filled
    lda $0700,x
    and #$E3            ; a sprite's third byte has no bits 2-4
    sta $0700,x
@filled:
    inx
    bne @fill
    lda #$00
    sta $2003
    lda #$07
    sta $4014
    ldx #$00
@check:
    stx $2003
    lda $2004
    cmp $0700,x
    bne @fail
    inx
    bne @check
"""),
    ("0L", "RAM mirrors ($0800, $1000, $1800) and PPU register mirrors ($2008-$3FFF)", r"""
    lda #$A7
    sta $02
    lda $0802
    cmp #$A7
    bne @fail
    lda #$7A
    sta $1803
    lda $03
    cmp #$7A
    bne @fail
    lda $1003
    cmp #$7A
    bne @fail
    bit $3FFA           ; $2002
    lda #$21
    sta $200E           ; $2006
    lda #$80
    sta $3FFE           ; $2006
    lda #$C9
    sta $200F           ; $2007
    bit $2002
    lda #$21
    sta $2006
    lda #$80
    sta $2006
    lda $2007
    lda $3FFF           ; $2007
    cmp #$C9
    bne @fail
"""),
    ("FH", "Open bus: $4018 and $5000 read as the last byte on the bus (the address's high byte)", r"""
    lda $4018
    cmp #$40
    bne @fail
    ldx $5000
    cpx #$50
    bne @fail
    ldy #$00
    lda $4018,y
    cmp #$40
    bne @fail
"""),
    ("1H", "Undocumented opcodes games use: LAX, SAX, DCP, ISC, SLO", r"""
.unofficial
    lda #$5C
    sta $00
    lda #$00
    tax
    lax $00             ; A and X from memory
    cmp #$5C
    bne @fail
    cpx #$5C
    bne @fail
    lda #$F0
    ldx #$3C
    sax $01             ; A AND X to memory
    lda $01
    cmp #$30
    bne @fail
    lda #$41
    sta $02
    lda #$40
    dcp $02             ; DEC, then CMP
    bne @fail
    bcc @fail
    lda $02
    cmp #$40
    bne @fail
    lda #$0F
    sta $03
    lda #$30
    sec
    isc $03             ; INC, then SBC
    cmp #$20
    bne @fail
    lda $03
    cmp #$10
    bne @fail
    lda #$81
    sta $04
    lda #$01
    slo $04             ; ASL, then ORA
    bcc @fail
    cmp #$03
    bne @fail
    lda $04
    cmp #$02
    bne @fail
    lda #$77
    sta $0480
    ldy #$80
    lax $0400,y
    cpx #$77
    bne @fail
    lda #$10
    sta $0481
    lda #$0F
    ldx #$01
    dcp $0480,x
    bne @fail
    lda $0481
    cmp #$0F
    bne @fail
.official
"""),
    ("2L", "Undocumented NOPs with operands are two and three bytes long and change nothing", r"""
.unofficial
    ldx #$00
    lda #$33
    sec
    nop #$E8            ; $80. Each operand byte is an INX, should it be run
    nop $E8             ; $04
    nop $E8,x           ; $14
    nop $E8E8           ; $0C
    nop $E8E8,x         ; $1C
    .db $1A, $3A, $5A, $7A, $DA, $FA
    .db $44, $E8, $64, $E8, $89, $E8
    .db $34, $E8, $54, $E8, $74, $E8, $D4, $E8, $F4, $E8
    .db $3C, $E8, $E8, $5C, $E8, $E8, $7C, $E8, $E8, $DC, $E8, $E8, $FC, $E8, $E8
.official
    bcc @fail
    cpx #$00
    bne @fail
    cmp #$33
    bne @fail
"""),
    ("FL", "Timing: iterations of an 11-cycle loop between two NMIs fit 29,780.67 CPU cycles per frame", r"""
    bit $2002
    lda #$80
    sta $2000
    jsr wait_nmi
    bcs @off
    jsr count_frame
    lda #$00
    sta $2000
    stx $00
    sty $01
    cpy #$0A            ; 2,560 to 2,815 iterations; a frame less the NMI handler is about 2,704
    bne @fail
    jmp @done
@off:
    lda #$00
    sta $2000
    jmp @fail
@done:
"""),
    ("FH", "Timing: the same loop run from RAM counts the same, give or take two", r"""
    lda #lo(count_frame)
    sta $0C
    lda #hi(count_frame)
    sta $0D
    lda #$00
    sta $0E
    lda #$02
    sta $0F
    ldy #count_frame_end-count_frame
    jsr copy_block
    bit $2002
    lda #$80
    sta $2000
    jsr wait_nmi
    bcs @off
    jsr count_frame
    stx $00
    sty $01
    jsr $0200
    stx $02
    sty $03
    lda #$00
    sta $2000
    cpy #$0A
    bne @fail
    sec
    lda $00
    sbc $02
    tax
    lda $01
    sbc $03
    beq @positive
    cmp #$FF
    bne @fail
    cpx #$FE
    bcc @fail
    jmp @done
@positive:
    cpx #$03
    bcs @fail
    jmp @done
@off:
    lda #$00
    sta $2000
    jmp @fail
@done:
"""),
]

CONSTANTS = r"""
; NES recompiler checklist ROM. Generated by make_checklist_rom.py.
test_index = $E0
test_result = $E1
test_ptr = $E2
saved_s = $E4
nmi_count = $E5
irq_count = $E6
brk_count = $E7
irq_p = $E8
irq_x = $E9
irq_pcl = $EA
irq_pch = $EB
irq_4015 = $EC
brk_p = $ED
brk_pcl = $EE
brk_pch = $EF
brk_in_p = $F0
first_fail = $F1
tmp = $F2
text_pos = $F3
pad = $F4
frames = $F5
"""

# The fixed bank: $C000-$FFFF.
FIXED_CODE = r"""
.bank 3
.org $C000
; Offset 0 of every bank is a routine that names the bank; they differ in
; length and all are position-independent (bank 3's runs at $8000 as well).
bank_id:
    lda #$D3
    rts

reset:
    sei
    cld
    ldx #$FF
    txs
    inx
    stx $2000           ; NMI off
    stx $2001           ; rendering off
    stx $4010           ; DMC IRQ off
    stx $4015
    lda #$40
    sta $4017           ; frame interrupt off
    jsr mmc1_init
    bit $2002
reset_vbl1:
    bit $2002
    bpl reset_vbl1
reset_vbl2:
    bit $2002           ; the PPU is ready after two vertical blanks
    bpl reset_vbl2
    lda #$00
    tax
clear_ram:
    sta $00,x
    sta $0100,x
    sta $0200,x
    sta $0300,x
    sta $0400,x
    sta $0500,x
    sta $0600,x
    sta $0700,x
    inx
    bne clear_ram
    lda #$80
    sta $6000           ; blargg's protocol: running
    lda #$DE
    sta $6001
    lda #$B0
    sta $6002
    lda #$61
    sta $6003
    lda #$00
    sta $6004
    lda #$01
    sta test_index

next_test:
    ldx test_index
    cpx #NTESTS+1
    jeq finish
    lda test_bank-1,x
    jsr set_prg
    ldx test_index
    lda test_lo-1,x
    sta test_ptr
    lda test_hi-1,x
    sta test_ptr+1
    tsx
    stx saved_s
    jsr call_test
    sta test_result
    sei                 ; put the machine back, whatever the test left
    cld
    ldx saved_s
    txs
    lda #$00
    sta $2000
    sta $2001
    lda #$40
    sta $4017
    lda $4015
    jsr mmc1_init
    ldx test_index
    lda test_result
    sta $0300,x
    inc test_index
    jmp next_test
call_test:
    jmp (test_ptr)

finish:
    lda #NTESTS
    sta $02FE
    lda #$00
    sta first_fail
    sta text_pos
    ldx #$01
fin_scan:
    lda $0300,x
    cmp #$01
    beq fin_next
    lda first_fail
    bne fin_add
    stx first_fail
    ldy #$00
fin_prefix:
    lda fail_text,y
    sta $6004,y
    iny
    cpy #$04
    bne fin_prefix
    sty text_pos
fin_add:
    jsr fin_number
fin_next:
    inx
    cpx #NTESTS+1
    bne fin_scan
    lda first_fail
    bne fin_terminate
    ldy #$00
fin_pass:
    lda pass_text,y
    sta $6004,y
    beq fin_show
    iny
    bne fin_pass
fin_terminate:
    ldy text_pos
    lda #$00
    sta $6004,y
fin_show:
    bit $2002           ; the backdrop: green for a pass, red otherwise
    lda #$3F
    sta $2006
    lda #$00
    sta $2006
    ldx #$2A
    lda first_fail
    beq fin_colour
    ldx #$16
fin_colour:
    stx $2007
    lda #$00
    sta $2006
    sta $2006
    lda first_fail
    sta $6000           ; the verdict, last of all
forever:
    jmp forever

; Appends " n" (X in decimal, below 100) to the text. Keeps X.
fin_number:
    txa
    pha
    ldy text_pos
    cpy #$E8
    bcs fin_number_done
    lda #$20
    sta $6004,y
    iny
    txa
    ldx #$2F
    sec
fin_tens:
    inx
    sbc #10
    bcs fin_tens
    adc #10
    sta tmp
    cpx #$30
    beq fin_ones
    txa
    sta $6004,y
    iny
fin_ones:
    lda tmp
    ora #$30
    sta $6004,y
    iny
    sty text_pos
fin_number_done:
    pla
    tax
    rts

pass_text:
    .db "ALL PASS", 0
fail_text:
    .db "FAIL"

; ---- interrupt handlers ----------------------------------------------------
nmi_handler:
    inc nmi_count
    rti

; IRQ and BRK share a vector; bit 4 of the pushed flags tells them apart.
irq_handler:
    pha
    txa
    pha
    tsx
    lda $0103,x
    and #$10
    beq irq_is_irq
    inc brk_count
    lda $0103,x
    sta brk_p
    lda $0104,x
    sta brk_pcl
    lda $0105,x
    sta brk_pch
    php
    pla
    sta brk_in_p
    jmp irq_exit
irq_is_irq:
    inc irq_count
    lda $0103,x
    sta irq_p
    lda $0101,x         ; X when the interrupt was taken
    sta irq_x
    lda $0104,x
    sta irq_pcl
    lda $0105,x
    sta irq_pch
    lda $4015           ; reading acknowledges the frame interrupt
    sta irq_4015
    lda irq_count
    cmp #$08
    bcc irq_exit
    lda #$40            ; it was not acknowledged: stop the storm, the test fails on the count
    sta $4017
irq_exit:
    pla
    tax
    pla
    rti

; ---- MMC1 -------------------------------------------------------------------
mmc1_init:
    lda #$80
    sta $8000           ; reset the shift register: last bank fixed at $C000
    lda #$0E            ; vertical mirroring, PRG mode 3, 8 KiB CHR
    jsr set_ctrl
    lda #$00
    jsr set_chr0
    lda #$00
set_prg:                ; A = the bank for $8000 (PRG RAM stays enabled)
@SERIAL_E000@
    rts
set_ctrl:
@SERIAL_8000@
    rts
set_chr0:
@SERIAL_A000@
    rts

; ---- helpers ----------------------------------------------------------------
; Waits for the vertical blank flag. Carry set after about twelve frames
; without one.
wait_vbl:
    ldx #$00
    ldy #$00
wait_vbl_loop:
    bit $2002
    bmi wait_vbl_ok
    inx
    bne wait_vbl_loop
    iny
    bpl wait_vbl_loop
    sec
    rts
wait_vbl_ok:
    clc
    rts

; Waits for the next NMI. Carry set after about twenty frames without one.
wait_nmi:
    lda nmi_count
    ldx #$00
    ldy #$00
wait_nmi_loop:
    cmp nmi_count
    bne wait_nmi_ok
    inx
    bne wait_nmi_loop
    iny
    bne wait_nmi_loop
    sec
    rts
wait_nmi_ok:
    clc
    rts

; About 41,000 CPU cycles: more than one APU frame (29,830).
delay_frame:
    ldy #$20
    ldx #$00
delay_loop:
    dex
    bne delay_loop
    dey
    bne delay_loop
    rts

; The controller: A, B, Select, Start, Up, Down, Left, Right from bit 7 down.
read_pad:
    lda #$01
    sta $4016
    lda #$00
    sta $4016
    ldx #$08
read_pad_bit:
    lda $4016
    lsr a
    rol pad
    dex
    bne read_pad_bit
    lda pad
    rts

; Copies Y bytes from ($0C) to ($0E).
copy_block:
    dey
    lda ($0C),y
    sta ($0E),y
    cpy #$00
    bne copy_block
    rts

copy_ram_add:
    lda #lo(ram_add_src)
    sta $0C
    lda #hi(ram_add_src)
    sta $0D
    lda #$00
    sta $0E
    lda #$02
    sta $0F
    ldy #ram_add_end-ram_add_src
    jmp copy_block

copy_ram_mmc:
    lda #lo(ram_mmc_src)
    sta $0C
    lda #hi(ram_mmc_src)
    sta $0D
    lda #$00
    sta $0E
    lda #$02
    sta $0F
    ldy #ram_mmc_end-ram_mmc_src
    jmp copy_block

copy_pram:
    lda #lo(pram_src)
    sta $0C
    lda #hi(pram_src)
    sta $0D
    lda #$00
    sta $0E
    lda #$61
    sta $0F
    ldy #pram_end-pram_src
    jmp copy_block

rom_inc_a:
    clc
    adc #$01
    rts

; Adds the two bytes that follow the JSR to $02 and returns past them.
add_inline:
    pla
    sta $00
    pla
    sta $01             ; ($00) is the JSR's last byte
    ldy #$01
    lda ($00),y
    clc
    adc $02
    iny
    clc
    adc ($00),y
    sta $02
    lda $00
    clc
    adc #$02
    tax
    lda $01
    adc #$00
    pha
    txa
    pha
    rts

jmpbug_go:
    jmp ($04FF)
ind_go:
    jmp ($0010)
ind_fixed:
    lda #$73
    rts

rom_ff:
    .db $FF
rom_7f:
    .db $7F
"""

# Placed code of the fixed bank. '@FH_TESTS@' is where the tests of the
# $E000 page go.
FIXED_PLACED = r"""
; ---- across $DFFF/$E000 -----------------------------------------------------
.org $DFE0
fx_cross:
    lda #$00
    sta $05
    clc
    bcc fx_cross_fwd    ; forward into the next 8 KiB page
fx_cross_back:
    lda #$10
    sta $04
    inc $05
    lda #$21
    clc
    .pad $DFFF
fx_cross_op:
    adc.w $0004         ; opcode at $DFFF, operand at $E000 and $E001
    sta $06             ; $E002: straight on
    rts
fx_cross_fwd:
    inc $05
    sec
    bcs fx_cross_back   ; back into the previous page

.org $E040
@FH_TESTS@

test_lo:
@TEST_LO@
test_hi:
@TEST_HI@
test_bank:
@TEST_BANK@

; ---- code that runs in RAM --------------------------------------------------
ram_add_src:
.rorg $0200
ram_add:
    lda #$11
    clc
    adc #$22
    jmp ram_add_tail
    lda #$EE
    rts
ram_add_tail:
    jsr rom_inc_a
    rts
ram_self:               ; returns its operand, then increments it
    lda #$00
    inc ram_self+1
    rts
ram_patch_next:         ; writes the next instruction just before running it
    ldx #$00
    lda #$E8            ; INX
    sta ram_patch_slot
ram_patch_slot:
    nop
    txa
    rts
.rend
ram_add_end:

; Bank-mode tests run from RAM: in modes 0-2 the fixed bank's code is not
; where it was.
ram_mmc_src:
.rorg $0200
ram_set_ctrl:
@SERIAL_8000@
    rts
ram_set_prg:
@SERIAL_E000@
    rts
ram_mode2:
    lda #$0A            ; PRG mode 2: bank 0 fixed at $8000
    jsr ram_set_ctrl
    lda #$01
    jsr ram_set_prg
    jsr $C000
    sta $00
    jsr $8000
    sta $01
    lda #$02
    jsr ram_set_prg
    jsr $C000
    sta $02
    jsr $8000
    sta $03
    lda #$03
    jsr ram_set_prg
    jsr $C000
    sta $04
ram_restore:
    lda #$0E            ; PRG mode 3 again, bank 0 at $8000
    jsr ram_set_ctrl
    lda #$00
    jsr ram_set_prg
    rts
ram_mode0:
    lda #$02            ; PRG mode 0: 32 KiB
    jsr ram_set_ctrl
    lda #$00
    jsr ram_set_prg
    jsr $8000
    sta $00
    jsr $C000
    sta $01
    lda #$01            ; the low bit is ignored
    jsr ram_set_prg
    jsr $8000
    sta $02
    jsr $C000
    sta $03
    lda #$02
    jsr ram_set_prg
    jsr $8000
    sta $04
    jsr $C000
    sta $05
    lda #$06            ; PRG mode 1 is the same
    jsr ram_set_ctrl
    lda #$03
    jsr ram_set_prg
    jsr $8000
    sta $06
    jsr $C000
    sta $07
    jmp ram_restore
ram_reset_mode:
    lda #$0A
    jsr ram_set_ctrl
    lda #$01
    jsr ram_set_prg
    jsr $C000
    sta $00
    jsr $8000
    sta $01
    lda #$80
    sta $8000           ; control OR $0C; the PRG register keeps its 1
    jsr $C000
    sta $02
    jsr $8000
    sta $03
    jmp ram_restore
.rend
ram_mmc_end:

pram_src:
.rorg $6100
pram_code:
    lda #$C0
    ora pram_data
    rts
pram_data:
    .db $03
.rend
pram_end:

; ---- placed targets ---------------------------------------------------------
; JMP ($04FF): the good target and the one a carry into the pointer's high
; byte would reach have the same low byte.
.org $F040
jmpbug_good:
    lda #$5A
    rts
.org $F140
jmpbug_bad:
    lda #$EE
    rts

; Counts iterations of an 11-cycle loop from one NMI to the next: X low, Y
; high. Position-independent and inside one 256-byte page, so no branch
; pays for a page crossing.
.org $F200
count_frame:
    lda nmi_count
count_frame_sync:
    cmp nmi_count
    beq count_frame_sync
    lda nmi_count
    ldx #$00
    ldy #$00
count_frame_loop:
    inx
    bne count_frame_same
    iny
count_frame_same:
    cmp nmi_count
    beq count_frame_loop
    rts
count_frame_end:
"""

# Placed code of the switchable banks.
BANK_CODE = {
    0: r"""
.bank 0
.org $8000
    lda #$A0
    rts
.org $8100
    lda #$01
    jsr set_prg         ; returns into bank 1
xcall_b0_ret:
    lda #$E0            ; bank 0 is no longer here
    rts
.org $8200
    lda #$02
@SERIAL_E000@
sw_b0_after:
    lda #$F0            ; bank 2 is here now
    rts
.org $8300
ind_b0:
    lda #$70
    rts
.org $9FF4
b0_straddle:
    lda #$00
    .pad $9FFE
b0_straddle_op:
    jsr b0_straddle_sub ; opcode at $9FFE, operand at $9FFF and $A000
    ora #$40            ; $A001
    rts
b0_straddle_sub:
    ora #$08
    tsx
    ldy $0101,x
    sty $00
    ldy $0102,x
    sty $01
    rts
""",
    1: r"""
.bank 1
.org $8000
    ldx #$B1
    txa
    rts
.org $8100
    lda #$01
    jsr set_prg
xcall_b1_mid:
    lda #$02            ; bank 0's call returns here
    jsr set_prg         ; returns into bank 2
xcall_b1_ret:
    lda #$E1
    rts
.org $8200
    lda #$02
@SERIAL_E000@
    lda #$01
@SERIAL_E000@
sw_b1_end:
    lda #$F1            ; bank 2's switch arrives here
    rts
.org $8300
ind_b1:
    lda #$71
    rts
.org $9FF0
b1_cross:
    lda #$00
    clc
    bcc b1_cross_fwd    ; forward into the next 8 KiB page
b1_cross_back:
    ora #$02
    ldx #$00
    .pad $9FFF
b1_cross_last:
    inx                 ; $9FFF, the page's last byte
    inx                 ; $A000
    cpx #$02
    bne b1_cross_bad
    ora #$04
    rts
b1_cross_bad:
    lda #$EE
    rts
b1_cross_fwd:
    ora #$01
    sec
    bcs b1_cross_back   ; back into the previous page
.org $BE00
ind_b1_high:
    lda #$72
    rts
""",
    2: r"""
.bank 2
.org $8000
    lda #$C0
    ora #$02
    rts
.org $8100
    .pad $8105
    lda #$EE            ; where bank 0's call would return, were this bank 1
    rts
    .pad $810A
xcall_b2_end:
    lda #$E2            ; bank 1's call returns here
    rts
.org $8200
    lda #$02
@SERIAL_E000@
sw_b2_mid:
    lda #$01            ; bank 0's switch arrives here: on to bank 1
@SERIAL_E000@
sw_b2_after:
    lda #$F2
    rts
.org $8300
    lda #$EE
    rts
.org $9EF0
pagex:
    lda #$00
    clc
    bcc pagex_fwd       ; forward across $9EFF/$9F00
pagex_back:
    ora #$02
    sec
    bcs pagex_end
    .pad $9F04
pagex_fwd:
    ora #$01
    clc
    bcc pagex_back      ; backward across it
    .pad $9F10
pagex_end:
    rts
.org $9FF8
b2_straddle:
    ldx #$00
    .pad $9FFF
b2_straddle_op:
    lda #$C3            ; opcode at $9FFF, operand at $A000
    inx                 ; $A001
    rts
""",
}

# Every bank ends with the same stub: the MMC1 may power up with any bank at
# $C000. The write resets the mapper, bank 3 arrives underneath, and the JMP
# is read from it.
STUB = r"""
.org $FFF0
reset_stub_b@BANK@:
    sei
    ldx #$FF
    txs
    stx $8000
    jmp reset
.org $FFFA
    .dw @NMI@, reset_stub_b@BANK@, @IRQ@
"""

SLOT_BANK = {"0": 0, "1": 1, "2": 2, "F": 3}
SLOT_ORG = {"L": 0x8400, "H": 0xA100}


def expand(text: str) -> str:
    for register in (0x8000, 0xA000, 0xE000):
        text = text.replace(f"@SERIAL_{register:04X}@", serial(register))
    return text


def table_lines(tests: list[dict]) -> list[str]:
    return [f"    {t['number']:>2}  {t['slot']}  {t['description']}" for t in tests]


def build_source(only: set[int] | None = None) -> tuple[str, list[dict]]:
    meta: list[dict] = []
    bodies: dict[str, list[str]] = {}
    for index, (slot, text, body) in enumerate(TESTS, 1):
        if only is not None and index not in only:
            continue
        number = len(meta) + 1
        label = f"t{number:02d}"
        # Failure exits may be far away: the inverse branch over a JMP. The
        # branch under test still decides the outcome.
        body = expand(body)
        for b in ("bne", "beq", "bcc", "bcs", "bmi", "bpl", "bvc", "bvs"):
            body = body.replace(f"    {b} @fail", f"    j{b[1:]} @fail").replace(f"    {b} @off", f"    j{b[1:]} @off")
        body = body.replace("@fail", f"{label}_fail").replace("@", f"{label}_")
        bodies.setdefault(slot, []).append(
            f"\n; test {number}: {text}\n{label}:{body}    lda #$01\n    rts\n{label}_fail:\n    lda #$FF\n    rts\n"
            f"{label}_end:\n")
        meta.append({"number": number, "test": index, "slot": slot, "bank": SLOT_BANK[slot[0]],
                     "ram": RESULTS + number, "description": text})
    if not meta:
        raise AsmError("no tests selected")
    if len(meta) > 99:
        raise AsmError("the result text holds two-digit test numbers")

    def tests_of(slot: str) -> str:
        return "".join(bodies.get(slot, []))

    out = [CONSTANTS, f"NTESTS = {len(meta)}", expand(FIXED_CODE), tests_of("FL")]
    placed = expand(FIXED_PLACED).replace("@FH_TESTS@", tests_of("FH"))
    for key, fn in (("LO", "lo"), ("HI", "hi")):
        placed = placed.replace(f"@TEST_{key}@", "\n".join(f"    .db {fn}(t{t['number']:02d})" for t in meta))
    # A fixed-bank test is entered with bank 0 at $8000.
    placed = placed.replace("@TEST_BANK@", "\n".join(f"    .db {t['bank'] % 3}" for t in meta))
    out.append(placed)
    out.append(STUB.replace("@BANK@", "3").replace("@NMI@", "nmi_handler").replace("@IRQ@", "irq_handler"))
    for bank in (0, 1, 2):
        out.append(expand(BANK_CODE[bank]))
        for half in "LH":
            out.append(f".org ${SLOT_ORG[half]:04X}")
            out.append(tests_of(f"{bank}{half}"))
        stub = f"reset_stub_b{bank}"
        out.append(STUB.replace("@BANK@", str(bank)).replace("@NMI@", stub).replace("@IRQ@", stub))
    return "\n".join(out) + "\n", meta


def check_layout(symbols: dict[str, int], label_bank: dict[str, int], meta: list[dict]) -> None:
    """The addresses the tests are about. A change that moves one fails the build."""
    def need(ok: bool, what: str) -> None:
        if not ok:
            raise AsmError("layout: " + what)

    s = symbols
    need(s["fx_cross_op"] == 0xDFFF, "the fixed bank's straddling instruction is not at $DFFF")
    need(s["b0_straddle_op"] == 0x9FFE, "bank 0's straddling JSR is not at $9FFE")
    need(s["b1_cross_last"] == 0x9FFF, "bank 1's last one-byte instruction is not at $9FFF")
    need(s["b2_straddle_op"] == 0x9FFF, "bank 2's straddling instruction is not at $9FFF")
    need(s["b1_cross_fwd"] >= 0xA000 and s["b1_cross_back"] < 0xA000, "bank 1's branches do not cross $A000")
    need(s["fx_cross_fwd"] >= 0xE000 and s["fx_cross_back"] < 0xE000, "the fixed bank's branches do not cross $E000")
    need(s["pagex_fwd"] >> 8 != s["pagex_back"] >> 8, "pagex does not cross a page")
    need(s["xcall_b0_ret"] == s["xcall_b1_mid"], "bank 0's call does not return onto bank 1's code")
    need(s["xcall_b1_ret"] == s["xcall_b2_end"], "bank 1's call does not return onto bank 2's code")
    need(s["sw_b0_after"] == s["sw_b2_mid"], "bank 0's switch does not continue in bank 2's code")
    need(s["sw_b2_after"] == s["sw_b1_end"], "bank 2's switch does not continue in bank 1's code")
    need(s["ind_b0"] == s["ind_b1"] and label_bank["ind_b0"] != label_bank["ind_b1"],
         "the indirect targets of banks 0 and 1 are not at one address")
    need(s["ind_b1_high"] >> 13 != s["ind_b1"] >> 13, "ind_b1_high is not in the bank's other page")
    need(s["jmpbug_good"] & 0xFF == s["jmpbug_bad"] & 0xFF == 0x40 and s["jmpbug_good"] >> 8 != s["jmpbug_bad"] >> 8,
         "the JMP ($xxFF) targets do not share a low byte")
    need(s["count_frame"] >> 8 == (s["count_frame_end"] - 1) >> 8, "count_frame crosses a page")
    for name in ("ram_add", "ram_mmc"):
        need(s[f"{name}_end"] - s[f"{name}_src"] <= 0xF0, f"{name} does not fit below the results at $02FE")
    need(s["ram_add"] == 0x0200 and s["ram_set_ctrl"] == 0x0200 and s["pram_code"] == 0x6100, "RAM code moved")
    need(s["bank_id"] == 0xC000 and s["reset"] >= 0xC000 and label_bank["reset"] == 3, "reset is not in the fixed bank")
    need(len({s[f"reset_stub_b{b}"] for b in range(BANKS)}) == 1, "the reset stubs are not at one address")
    for t in meta:
        label = f"t{t['number']:02d}"
        start, end = s[label], s[label + "_end"]
        need(label_bank[label] == t["bank"], f"test {t['number']} is in the wrong bank")
        need(start >> 13 == (end - 1) >> 13, f"test {t['number']} crosses an 8 KiB page")
        want = {"L": 0x8000, "H": 0xA000}.get(t["slot"][1])
        if t["slot"][0] == "F":
            want = 0xC000 if t["slot"][1] == "L" else 0xE000
        need(start & 0xE000 == want, f"test {t['number']} is not in page {t['slot']}")
        t["address"] = start


def check_docstring() -> None:
    all_tests = [{"number": i, "slot": slot, "description": text} for i, (slot, text, _) in enumerate(TESTS, 1)]
    missing = [line for line in table_lines(all_tests) if line not in __doc__]
    if missing:
        raise AsmError("the module docstring's table is out of date; it should have:\n" + "\n".join(missing))


def build(output: Path, json_path: Path | None = None, asm_path: Path | None = None,
          only: set[int] | None = None) -> list[dict]:
    check_docstring()
    source, meta = build_source(only)
    if asm_path:
        asm_path.write_text(source)
    asm = Assembler(BANKS)
    lines = source.splitlines()
    asm.assemble(lines, final=False)
    asm.assemble(lines, final=True)
    check_layout(asm.symbols, asm.label_bank, meta)
    output.parent.mkdir(parents=True, exist_ok=True)
    write_ines(output, bytes(asm.image), mapper=MAPPER, mirroring="vertical", prg_ram=0x2000)
    if json_path:
        json_path.write_text(json.dumps({
            "rom": {"mapper": MAPPER, "prg_banks": BANKS, "chr": "RAM", "prg_ram": 0x2000},
            "runner_args": ["--blargg", "--frames", str(FRAMES), *KEYS],
            "keys": KEYS,
            "frames": FRAMES,
            "results": {"base": RESULTS, "count": 0x02FE, "pass": 1, "fail": 0xFF, "not_reached": 0},
            "tests": meta,
        }, indent=2) + "\n")
    return meta


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--asm", type=Path, help="also write the generated assembly source")
    parser.add_argument("--only", help="comma-separated test numbers (for debugging; they are renumbered from 1)")
    parser.add_argument("--list", action="store_true", help="print the test table and exit")
    args = parser.parse_args()
    if args.list:
        print("\n".join(table_lines([{"number": i, "slot": slot, "description": text}
                                     for i, (slot, text, _) in enumerate(TESTS, 1)])))
        return 0
    if not args.output:
        parser.error("--output is required")
    only = {int(v) for v in args.only.split(",")} if args.only else None
    try:
        build(args.output, args.json, args.asm, only)
    except AsmError as error:
        print(f"assembly failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
