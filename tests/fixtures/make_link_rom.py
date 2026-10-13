"""Game Boy link cable test ROM (test_gb_link.c, manual two-window runs).

    make_link_rom.py OUT.gb

The side whose player holds START, or whose HRAM FF86 is non-zero (set by a
test), drives the clock: it sends $A0+n, waits for the transfer to end,
checks what came back, pauses a little as a game's loop would, and repeats.
The other side waits with the external clock, sending $40+(n&$3F) (never
$FF, which means "nobody answered"). HRAM:
  FF80 transfers completed, FF81 last byte received, FF82 role (1 drives the
  clock, 2 waits), FF83 bytes received that were wrong, FF84 last wrong
  byte, FF85 good replies the clock side has had.
"""
import sys

code = []
labels = {}
fixups = []

def emit(*bs):
    code.extend(bs)

def label(name):
    labels[name] = 0x150 + len(code)

def jr(op, target):
    emit(op, 0)
    fixups.append((len(code) - 1, target))

def jp(target):
    emit(0xC3, 0, 0)
    fixups.append((len(code) - 2, ('abs', target)))

def ldh_w(addr, value=None):
    if value is not None:
        emit(0x3E, value)
    emit(0xE0, addr)

emit(0xF3)                       # di
emit(0x31, 0xFE, 0xFF)           # ld sp,$FFFE
emit(0xAF)                       # xor a
for a in (0x80, 0x81, 0x82, 0x83, 0x84, 0x85):
    emit(0xE0, a)                # ldh [a],a
label('main')
emit(0xF0, 0x86, 0xB7)           # ldh a,[$86]; or a
jr(0x20, 'master')               # a test chose this side
ldh_w(0x00, 0x10)                # select action buttons
emit(0xF0, 0x00, 0xF0, 0x00)     # ldh a,[$00] x2
emit(0xCB, 0x5F)                 # bit 3,a (START, active low)
jr(0x20, 'slave')                # jr nz,slave
label('master')

# master
ldh_w(0x82, 0x01)
emit(0xF0, 0x80)                 # ldh a,[$80]
emit(0xC6, 0xA0)                 # add $A0
emit(0xE0, 0x01)                 # SB
ldh_w(0x02, 0x81)                # SC: start, internal clock
label('wait_m')
emit(0xF0, 0x02, 0xCB, 0x7F)     # ldh a,[$02]; bit 7,a
jr(0x20, 'wait_m')
emit(0xF0, 0x01, 0xE0, 0x81)     # received -> FF81
emit(0xFE, 0xFF)                 # cp $FF
jr(0x28, 'no_reply')             # slave not waiting yet: try again
# expected = $50 + FF85 (count of good replies)
emit(0x47)                       # ld b,a
emit(0xF0, 0x85, 0xE6, 0x3F, 0xC6, 0x40)  # ldh a,[$85]; and $3F; add $40
emit(0xB8)                       # cp b
jr(0x28, 'good')
emit(0x21, 0x83, 0xFF, 0x34)     # ld hl,$FF83; inc [hl]
emit(0x78, 0xE0, 0x84)           # ld a,b; ldh [$84],a
label('good')
emit(0x21, 0x85, 0xFF, 0x34)     # good replies++ (the slave counts these too)
emit(0x21, 0x80, 0xFF, 0x34)     # transfers++
label('no_reply')
emit(0x01, 0x00, 0x02)           # ld bc,$0200: a short pause, as a game's loop would make
label('delay')
emit(0x0B, 0x78, 0xB1)           # dec bc; ld a,b; or c
jr(0x20, 'delay')
jp('main')

# slave
label('slave')
ldh_w(0x82, 0x02)
emit(0xF0, 0x80, 0xE6, 0x3F, 0xC6, 0x40)  # $40 + (transfers & $3F): never $FF
emit(0xE0, 0x01)
ldh_w(0x02, 0x80)                # SC: wait for the other side's clock
label('wait_s')
emit(0xF0, 0x02, 0xCB, 0x7F)
jr(0x28, 'sdone')
emit(0xF0, 0x86, 0xB7)           # the test made this side the master
jr(0x20, 'to_master')
emit(0xF0, 0x00, 0xCB, 0x5F)     # START now held: become the master
jr(0x20, 'wait_s')
label('to_master')
ldh_w(0x02, 0x00)
jp('main')
label('sdone')
emit(0xF0, 0x01, 0xE0, 0x81)     # received -> FF81
emit(0x47)                       # ld b,a
emit(0xF0, 0x80, 0xC6, 0xA0)     # expected $A0 + transfers
emit(0xB8)
jr(0x28, 'sgood')
emit(0x21, 0x83, 0xFF, 0x34)
emit(0x78, 0xE0, 0x84)
label('sgood')
emit(0x21, 0x80, 0xFF, 0x34)
jp('main')

for pos, target in fixups:
    if isinstance(target, tuple):
        addr = labels[target[1]]
        code[pos] = addr & 0xFF
        code[pos + 1] = addr >> 8
    else:
        rel = labels[target] - (0x150 + pos + 1)
        assert -128 <= rel <= 127, target
        code[pos] = rel & 0xFF

rom = bytearray(0x8000)
rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
rom[0x134:0x134 + 8] = b'LINKTEST'
rom[0x147] = 0x00  # ROM only
rom[0x148] = 0x00
rom[0x149] = 0x00
rom[0x14A] = 0x01
chk = 0
for b in rom[0x134:0x14D]:
    chk = (chk - b - 1) & 0xFF
rom[0x14D] = chk
rom[0x150:0x150 + len(code)] = bytes(code)
g = sum(rom) - rom[0x14E] - rom[0x14F]
rom[0x14E] = (g >> 8) & 0xFF
rom[0x14F] = g & 0xFF
out = sys.argv[1] if len(sys.argv) > 1 else 'linktest.gb'
open(out, 'wb').write(rom)
print(out, len(code), 'bytes of code')
