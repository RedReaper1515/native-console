/*
 * SA-1 (Super Accelerator) cartridge coprocessor: a second 65C816 at
 * 10.74 MHz (2 master clocks per bus cycle) with 2 KiB of I-RAM, the
 * cartridge's BW-RAM (battery RAM, m->sram), Super MMC ROM banking, DMA with
 * character conversion, a multiplier/divider/accumulator, a variable-length
 * bit reader, an H/V timer, and interrupt lines both ways.
 *
 * The SA-1 runs the same instruction handlers as the S-CPU (snes_cpu_impl.h)
 * in its own SnesCpu (id 1), so recompiled ROM code serves both CPUs. It
 * lags the S-CPU and is caught up (snes_sa1_run) before every S-CPU access
 * to shared state (SA-1 registers, I-RAM, BW-RAM) and whenever S-CPU events
 * run, so each CPU sees the other's writes in time order; the SA-1 cannot
 * see WRAM or the PPU, so nothing else is shared.
 *
 * Behaviour follows ares (ISC license, Near et al.); see THIRD_PARTY.md.
 * Not modelled: bus conflict wait states (the SA-1 waiting while the S-CPU
 * uses the same ROM, BW-RAM or I-RAM).
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 */
#ifndef SNES_SA1_H
#define SNES_SA1_H

#include <stdbool.h>
#include <stdint.h>

#include "snes_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SnesMachine;

typedef struct SnesSa1 {
    SnesCpu cpu; /* id 1 */
    uint8_t iram[0x800];
    bool present;
    bool running;      /* inside snes_sa1_run */
    bool int_pending;  /* an interrupt is taken before the next instruction */
    uint16_t vector;   /* ... at this address (bank 0) */

    /* H/V timer: counters were (timer_h0, timer_v0) at SA-1 time timer_base. */
    uint64_t timer_base;
    uint16_t timer_h0, timer_v0;
    uint64_t timer_next; /* next timer IRQ, UINT64_MAX = none */

    bool bwram_dma;    /* character conversion type 1 is active */
    uint8_t dma_line;  /* character conversion type 2 line */

    /* $2200 CCNT */
    bool sa1_irq, sa1_rdyb, sa1_resb, sa1_nmi;
    uint8_t smeg;
    /* $2201 SIE / $2202 SIC */
    bool cpu_irqen, chdma_irqen, cpu_irqcl, chdma_irqcl;
    uint16_t crv, cnv, civ; /* $2203-$2208 SA-1 vectors */
    /* $2209 SCNT */
    bool cpu_irq, cpu_ivsw, cpu_nvsw;
    uint8_t cmeg;
    /* $220A CIE / $220B CIC */
    bool sa1_irqen, timer_irqen, dma_irqen, sa1_nmien;
    bool sa1_irqcl, timer_irqcl, dma_irqcl, sa1_nmicl;
    uint16_t snv, siv; /* $220C-$220F S-CPU vectors */
    /* $2210 TMC, $2212-$2215 */
    bool hvselb, ven, hen;
    uint16_t hcnt, vcnt;
    /* $2220-$2223 Super MMC */
    uint8_t mmc_bank[4];
    bool mmc_mode[4];
    bool mmc_linear;   /* banks C-F hold 0-3: the ROM map the recompiler assumed */
    /* $2224-$222A BW-RAM and I-RAM mapping and protection */
    uint8_t sbm;
    bool sw46;
    uint8_t cbm;
    bool swen, cwen;
    uint8_t bwp, siwp, ciwp;
    /* $2230-$2239 DMA */
    bool dmaen, dprio, cden, cdsel, dd;
    uint8_t sd;
    bool chdend;
    uint8_t dmasize, dmacb;
    uint32_t dsa, dda;
    uint16_t dtc;
    /* $223F, $2240-$224F bitmap */
    bool bbf;
    uint8_t brf[16];
    /* $2250-$2254 arithmetic */
    bool acm, md;
    uint16_t ma, mb;
    uint64_t mr; /* 40 bits */
    bool overflow;
    /* $2258-$225B variable-length bit reader */
    bool hl;
    uint8_t vb;
    uint32_t va;
    uint8_t vbit;
    /* $2300 SFR / $2301 CFR flags */
    bool cpu_irqfl, chdma_irqfl;
    bool sa1_irqfl, timer_irqfl, dma_irqfl, sa1_nmifl;
    uint16_t hcr, vcr;
} SnesSa1;

/* Cartridge setup (snes_cart_map): map the S-CPU's view of an SA-1 board. */
void snes_sa1_map(struct SnesMachine *m);
/* Power on: the SA-1 is held in reset until the S-CPU releases it. */
void snes_sa1_power(struct SnesMachine *m);
/* Run the SA-1 until its clock reaches `target` (master clocks). */
void snes_sa1_run(struct SnesMachine *m, uint64_t target);
static inline void snes_sa1_catch_up(struct SnesMachine *m, SnesSa1 *s, uint64_t now)
{
    if (s->present && s->cpu.cycles < now) snes_sa1_run(m, now);
}

/* The S-CPU side: SA-1 registers ($2200-$23FF), I-RAM ($3000-$37FF),
 * BW-RAM ($6000-$7FFF window, banks 40-4F) and the vector override. */
uint8_t snes_sa1_cpu_read(struct SnesMachine *m, uint32_t addr, uint8_t mdr);
void snes_sa1_cpu_write(struct SnesMachine *m, uint32_t addr, uint8_t v);
/* Side-effect-free read of the S-CPU view, for debuggers. */
uint8_t snes_sa1_peek(const struct SnesMachine *m, uint32_t addr);

/* The SA-1's own bus (snes_bus_read etc. dispatch here for cpu id 1). */
uint8_t snes_sa1_bus_read(SnesCpu *c, uint32_t addr);
void snes_sa1_bus_write(SnesCpu *c, uint32_t addr, uint8_t v);
void snes_sa1_bus_idle(SnesCpu *c);
void snes_sa1_idle_jump(SnesCpu *c);

#ifdef __cplusplus
}
#endif

#endif /* SNES_SA1_H */
