/*
 * SNES APU: the SPC700 over the audio memory map, the SMP I/O registers
 * ($F0-$FF), the three timers, the S-DSP and the S-CPU ports.
 *
 * The memory map, I/O register behaviour, wait-state costs and timer
 * stages are ported from ares (ISC license, Near et al.),
 * ares/sfc/smp/{memory,io,timing,smp}.cpp at commit
 * af4cbb04f067682a8a3cf42695ff78bed634b38d. Differences from that code:
 *  - timer 2 is reset on a 0->1 CONTROL transition like timers 0 and 1
 *    (ares resets it on every write that is not one: a typo there;
 *    fullsnes and every other description treat the three alike);
 *  - power state is deterministic (fixed RAM pattern, fullsnes reset
 *    values) instead of random;
 *  - the boot ROM is our own program (below), not Nintendo's IPL.
 *
 * Timing: every SMP bus cycle first advances the DSP by one clock and the
 * timers, then performs the access (the order ares's threads produce).
 * With TEST's wait-state fields at 0 (the only setting software uses)
 * every cycle costs one SMP cycle.
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 */
#include "snes_apu.h"

#include <string.h>

/* ---- Boot ROM ($FFC0-$FFFF) ----------------------------------------------
 *
 * Our own 64-byte program implementing the documented upload protocol
 * (fullsnes, "SNES APU Main CPU Communication Port"):
 *  - reset: SP = $EF, clear $0001-$00EF, write $AA/$BB to ports 0/1;
 *  - wait until port 0 reads $CC (the first kick);
 *  - command: address from ports 2/3 (stored at $0000/$0001), command
 *    from port 1, echo the kick on port 0. Command 0 jumps to the address
 *    with A = X = Y = 0, SP = $EF, PSW = $02. Otherwise a block upload:
 *    wait for index 0 on port 0, then for each byte, when port 0 equals
 *    the expected index, take the data from port 1, echo the index on
 *    port 0 and store it; when port 0 moves past the expected index (the
 *    next kick, index + 2 or more) a new command begins.
 * $FFC0 (reset) and $FFC9 (handshake without clearing) are both usable
 * entry points, as with the original. Port 1 keeps $BB throughout.
 * The program starts with MOV X,#$EF because software checks for it:
 * blargg's SPC tests read $FFC0 and expect $CD before jumping back into
 * the boot ROM.
 */
static const uint8_t boot_rom[64] = {
    /* FFC0 */ 0xCD, 0xEF,       /* mov  x,#$EF        ; software checks $FFC0 */
    /* FFC2 */ 0xBD,             /* mov  sp,x          ; SP = $EF              */
    /* FFC3 */ 0xE8, 0x00,       /* mov  a,#$00                               */
    /* FFC5 */ 0xC6,             /* mov  (x),a         ; clear $EF..$01        */
    /* FFC6 */ 0x1D,             /* dec  x                                    */
    /* FFC7 */ 0xD0, 0xFC,       /* bne  $FFC5                                */
    /* FFC9 */ 0xE8, 0xAA,       /* mov  a,#$AA        ; handshake entry       */
    /* FFCB */ 0x8D, 0xBB,       /* mov  y,#$BB                               */
    /* FFCD */ 0xDA, 0xF4,       /* movw $F4,ya        ; ports 0/1 = $AA/$BB   */
    /* FFCF */ 0xE8, 0xCC,       /* mov  a,#$CC                               */
    /* FFD1 */ 0x64, 0xF4,       /* cmp  a,$F4         ; wait for first kick   */
    /* FFD3 */ 0xD0, 0xFC,       /* bne  $FFD1                                */
    /* FFD5 */ 0xBA, 0xF6,       /* movw ya,$F6        ; command: address      */
    /* FFD7 */ 0xDA, 0x00,       /* movw $00,ya        ;   -> [$0000]          */
    /* FFD9 */ 0xE4, 0xF4,       /* mov  a,$F4         ; kick                  */
    /* FFDB */ 0xF8, 0xF5,       /* mov  x,$F5         ; command (sets Z)      */
    /* FFDD */ 0xC4, 0xF4,       /* mov  $F4,a         ; acknowledge the kick  */
    /* FFDF */ 0xD0, 0x06,       /* bne  $FFE7         ; non-zero: upload      */
    /* FFE1 */ 0x7D,             /* mov  a,x           ; A = 0                 */
    /* FFE2 */ 0xFD,             /* mov  y,a           ; Y = 0, Z = 1, N = 0   */
    /* FFE3 */ 0x60,             /* clrc               ; PSW = $02             */
    /* FFE4 */ 0x1F, 0x00, 0x00, /* jmp  [!$0000+x]    ; X = 0: to the address */
    /* FFE7 */ 0xE4, 0xF4,       /* mov  a,$F4         ; upload: wait for      */
    /* FFE9 */ 0xD0, 0xFC,       /* bne  $FFE7         ;   index 0             */
    /* FFEB */ 0x5D,             /* mov  x,a           ; X = expected index    */
    /* FFEC */ 0xFD,             /* mov  y,a           ; Y = 0                 */
    /* FFED */ 0xE4, 0xF5,       /* mov  a,$F5         ; byte: data            */
    /* FFEF */ 0xD8, 0xF4,       /* mov  $F4,x         ; echo the index        */
    /* FFF1 */ 0xD7, 0x00,       /* mov  [$00]+y,a     ; store at [$0000]      */
    /* FFF3 */ 0x3A, 0x00,       /* incw $00           ; advance the pointer   */
    /* FFF5 */ 0x3D,             /* inc  x                                    */
    /* FFF6 */ 0x3E, 0xF4,       /* cmp  x,$F4         ; poll: X - port 0      */
    /* FFF8 */ 0xF0, 0xF3,       /* beq  $FFED         ; next byte is there    */
    /* FFFA */ 0x10, 0xFA,       /* bpl  $FFF6         ; port behind: wait     */
    /* FFFC */ 0x2F, 0xD7,       /* bra  $FFD5         ; port ahead: command   */
    /* FFFE */ 0xC0, 0xFF,       /* reset vector: $FFC0                       */
};

/* ---- Timers ---------------------------------------------------------------- */

/* Stage 2 counts falling edges of the gated stage-1 clock. */
static inline void timer_sync_stage1(const SnesApu *a, SnesApuTimer *t)
{
    uint8_t level = t->stage1;
    if (!(a->test & 0x08u)) level = 0; /* TEST bit 3: timers enable */
    if (a->test & 0x01u) level = 0;    /* TEST bit 0: timers disable */
    const uint8_t fell = (uint8_t)(t->line && !level);
    t->line = level;
    if (!fell || !t->enable) return;
    t->stage2 = (uint8_t)(t->stage2 + 1u);
    if (t->stage2 != t->target) return; /* target 0 = 256 */
    t->stage2 = 0;
    t->stage3 = (uint8_t)((t->stage3 + 1u) & 15u);
}

/* `clocks` in 2.048 MHz units; `half` is the stage-1 half period (64 for
 * 8 kHz, 8 for 64 kHz, in SMP cycles) times two. */
static inline void timer_step(const SnesApu *a, SnesApuTimer *t, unsigned clocks, unsigned half)
{
    t->stage0 = (uint8_t)(t->stage0 + clocks);
    if (t->stage0 < half) return;
    t->stage0 = (uint8_t)(t->stage0 - half);
    t->stage1 ^= 1u;
    timer_sync_stage1(a, t);
}

static inline void timers_step(SnesApu *a, unsigned clocks)
{
    timer_step(a, &a->timer[0], clocks, 128);
    timer_step(a, &a->timer[1], clocks, 128);
    timer_step(a, &a->timer[2], clocks, 16);
}

/* ---- Clocking -------------------------------------------------------------- */

/* One SMP cycle at normal speed. */
static inline void apu_tick(SnesApu *a)
{
    a->cycles++;
    snes_dsp_clock(&a->dsp, a->aram);
    timers_step(a, 2);
}

/* A bus cycle with TEST wait states: {1, 2, 5, 10} SMP cycles; the timers
 * advance by {1, 2, 4, 8} (the hardware's glitch at the two slow
 * settings). */
static void apu_wait_slow(SnesApu *a, unsigned ws)
{
    static const uint8_t cycles[4] = { 1, 2, 5, 10 };
    static const uint8_t timer_clocks[4] = { 2, 4, 8, 16 };
    for (unsigned i = 0; i < cycles[ws]; i++) {
        a->cycles++;
        snes_dsp_clock(&a->dsp, a->aram);
    }
    timers_step(a, timer_clocks[ws]);
}

static inline int is_io(uint16_t addr) { return (addr & 0xFFF0u) == 0x00F0u; }

static inline void apu_wait_access(SnesApu *a, uint16_t addr)
{
    if (!(a->test & 0xF0u)) {
        apu_tick(a);
        return;
    }
    unsigned ws = (a->test >> 4) & 3u; /* RAM */
    if (is_io(addr) || (addr >= 0xFFC0u && (a->control & 0x80u))) ws = a->test >> 6; /* I/O and ROM */
    apu_wait_slow(a, ws);
}

/* ---- I/O registers --------------------------------------------------------- */

static uint8_t io_read(SnesApu *a, uint16_t addr)
{
    switch (addr & 0x0Fu) {
    case 0x0: /* TEST (write-only) */
    case 0x1: /* CONTROL (write-only) */
        return 0x00;
    case 0x2: return a->dsp_addr;
    case 0x3: return snes_dsp_read(&a->dsp, (uint8_t)(a->dsp_addr & 0x7Fu)); /* $80-$FF mirror $00-$7F */
    case 0x4:
    case 0x5:
    case 0x6:
    case 0x7: return a->cpu_to_apu[addr & 3u];
    case 0x8: return a->aux[0];
    case 0x9: return a->aux[1];
    case 0xA:
    case 0xB:
    case 0xC: return 0x00; /* timer targets (write-only) */
    default: {             /* $FD-$FF: timer outputs, cleared on read */
        SnesApuTimer *t = &a->timer[(addr & 0x0Fu) - 0x0Du];
        uint8_t v = t->stage3;
        t->stage3 = 0;
        return v;
    }
    }
}

static void io_write(SnesApu *a, uint16_t addr, uint8_t v)
{
    switch (addr & 0x0Fu) {
    case 0x0: /* TEST: only writable with the P flag clear */
        if (a->smp.fp) break;
        a->test = v;
        for (int i = 0; i < 3; i++) timer_sync_stage1(a, &a->timer[i]);
        break;
    case 0x1: /* CONTROL */
        for (int i = 0; i < 3; i++) {
            const uint8_t en = (uint8_t)((v >> i) & 1u);
            if (en && !a->timer[i].enable) { /* 0->1 resets the counters */
                a->timer[i].stage2 = 0;
                a->timer[i].stage3 = 0;
            }
            a->timer[i].enable = en;
        }
        if (v & 0x10u) a->cpu_to_apu[0] = a->cpu_to_apu[1] = 0;
        if (v & 0x20u) a->cpu_to_apu[2] = a->cpu_to_apu[3] = 0;
        a->control = v;
        break;
    case 0x2: a->dsp_addr = v; break;
    case 0x3:
        if (!(a->dsp_addr & 0x80u)) snes_dsp_write(&a->dsp, a->dsp_addr, v); /* $80-$FF read-only */
        break;
    case 0x4:
    case 0x5:
    case 0x6:
    case 0x7: a->apu_to_cpu[addr & 3u] = v; break;
    case 0x8: a->aux[0] = v; break;
    case 0x9: a->aux[1] = v; break;
    case 0xA:
    case 0xB:
    case 0xC: a->timer[(addr & 0x0Fu) - 0x0Au].target = v; break;
    default: break; /* timer outputs are read-only */
    }
}

/* ---- The SPC700 over the APU bus ------------------------------------------- */

static inline uint8_t apu_read(SnesApu *a, uint16_t addr)
{
    apu_wait_access(a, addr);
    if (is_io(addr)) return io_read(a, addr);
    if (addr >= 0xFFC0u && (a->control & 0x80u)) return boot_rom[addr & 0x3Fu];
    if (a->test & 0x04u) return 0x5A; /* TEST bit 2: RAM disabled */
    return a->aram[addr];
}

static inline void apu_write(SnesApu *a, uint16_t addr, uint8_t v)
{
    apu_wait_access(a, addr);
    /* Every write reaches RAM (I/O and $FFC0-$FFFF included) unless TEST
     * disables RAM writes. */
    if ((a->test & 0x06u) == 0x02u) a->aram[addr] = v;
    if (is_io(addr)) io_write(a, addr, v);
}

static inline void apu_idle(SnesApu *a)
{
    if (!(a->test & 0xC0u)) apu_tick(a);
    else apu_wait_slow(a, a->test >> 6);
}

#define SPC_FN apu_step
#define SPC_CTX SnesApu *
#define SPC_READ(ctx, addr) apu_read((ctx), (addr))
#define SPC_WRITE(ctx, addr, v) apu_write((ctx), (addr), (v))
#define SPC_IDLE(ctx) apu_idle(ctx)
#include "snes_spc700_impl.h"

/* ---- Public interface ------------------------------------------------------ */

static void reset_common(SnesApu *a)
{
    memset(&a->smp, 0, sizeof a->smp);
    a->smp.sp = 0xEF;
    snes_spc700_set_psw(&a->smp, 0x02);
    a->smp.pc = (uint16_t)(boot_rom[0x3E] | (boot_rom[0x3F] << 8));

    memset(a->timer, 0, sizeof a->timer);
    for (int i = 0; i < 3; i++) a->timer[i].target = 0xFF; /* fullsnes reset values */
    a->test = 0x0A;
    a->control = 0xB0; /* boot ROM on; bits 4/5 cleared the input ports */
    a->dsp_addr = 0xFF;
    a->aux[0] = a->aux[1] = 0xFF;
    memset(a->cpu_to_apu, 0, sizeof a->cpu_to_apu);
    memset(a->apu_to_cpu, 0, sizeof a->apu_to_cpu);
}

void snes_apu_power(SnesApu *apu)
{
    memset(apu, 0, sizeof *apu);
    for (uint32_t i = 0; i < 0x10000u; i++) apu->aram[i] = (i & 0x20u) ? 0xFF : 0x00;
    snes_dsp_power(&apu->dsp);
    reset_common(apu);
}

void snes_apu_reset(SnesApu *apu)
{
    snes_dsp_reset(&apu->dsp);
    reset_common(apu);
}

uint32_t snes_apu_run(SnesApu *apu, uint32_t smp_cycles)
{
    const uint64_t start = apu->cycles;
    const uint64_t end = start + smp_cycles;
    while (apu->cycles < end) apu_step(&apu->smp, apu);
    return (uint32_t)(apu->cycles - start);
}

uint8_t snes_apu_cpu_read(const SnesApu *apu, unsigned port) { return apu->apu_to_cpu[port & 3u]; }

void snes_apu_cpu_write(SnesApu *apu, unsigned port, uint8_t value) { apu->cpu_to_apu[port & 3u] = value; }

size_t snes_apu_read_samples(SnesApu *apu, int16_t *out, size_t max_frames)
{
    return snes_dsp_read_samples(&apu->dsp, out, max_frames);
}

const uint8_t *snes_apu_ram(const SnesApu *apu) { return apu->aram; }
