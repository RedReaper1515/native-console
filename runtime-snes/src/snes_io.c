/*
 * S-CPU I/O registers: the WRAM port ($2180-$2183), the controller serial
 * ports ($4016/$4017), and $4200-$421F (interrupt enables, the programmable
 * I/O port, the multiplier and divider, H/V timers, DMA enables, ROM speed,
 * status and auto-joypad results).
 *
 * Multiplication and division are not instant: the 5A22 computes one step
 * per CPU cycle (8 for a multiply, 16 for a divide) and the result
 * registers show the partial state until then. Every CPU bus cycle bumps
 * m->alu_edges; the registers apply the pending steps when they are next
 * touched. The step algorithm follows ares (ISC).
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for
 *   any purpose with or without fee is hereby granted, provided that the
 *   above copyright notice and this permission notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 *   WARRANTIES WITH REGARD TO THIS SOFTWARE.
 */
#include "snes.h"

static void alu_sync(SnesMachine *m)
{
    uint64_t pending = m->alu_edges - m->alu_synced;
    m->alu_synced = m->alu_edges;
    while (pending-- && (m->alu_mul_left || m->alu_div_left)) {
        if (m->alu_mul_left) {
            m->alu_mul_left--;
            if (m->rddiv & 1u) m->rdmpy = (uint16_t)(m->rdmpy + m->alu_shift);
            m->rddiv >>= 1;
            m->alu_shift <<= 1;
        }
        if (m->alu_div_left) {
            m->alu_div_left--;
            m->rddiv = (uint16_t)(m->rddiv << 1);
            m->alu_shift >>= 1;
            if (m->rdmpy >= m->alu_shift) {
                m->rdmpy = (uint16_t)(m->rdmpy - m->alu_shift);
                m->rddiv |= 1u;
            }
        }
    }
}

/* Serial read of a controller: B, Y, Select, Start, Up, Down, Left, Right,
 * A, X, L, R, then four ID bits (0), then 1s. */
static uint8_t serial_bit(SnesMachine *m, int port)
{
    if (m->strobe) m->shift[port] = m->buttons[port];
    uint8_t bit = (uint8_t)((m->shift[port] >> 15) & 1u);
    if (!m->strobe) m->shift[port] = (uint16_t)((m->shift[port] << 1) | 1u);
    return bit;
}

uint8_t snes_io_read(SnesMachine *m, uint32_t addr)
{
    uint8_t mdr = m->cpu.mdr;
    switch (addr & 0xFFFFu) {
    case 0x2180: { /* WMDATA */
        uint8_t v = m->wram[m->wram_port];
        m->wram_port = (m->wram_port + 1u) & 0x1FFFFu;
        return v;
    }
    case 0x4016: return (uint8_t)((mdr & 0xFCu) | serial_bit(m, 0));
    case 0x4017: return (uint8_t)((mdr & 0xE0u) | 0x1Cu | serial_bit(m, 1));
    case 0x4210: { /* RDNMI: read clears the flag */
        uint8_t v = (uint8_t)((mdr & 0x70u) | (m->rdnmi ? 0x80u : 0u) | 0x02u);
        m->rdnmi = false;
        return v;
    }
    case 0x4211: { /* TIMEUP: read clears the flag and the IRQ line */
        uint8_t v = (uint8_t)((mdr & 0x7Fu) | (m->timeup ? 0x80u : 0u));
        m->timeup = false;
        snes_update_irq(m);
        return v;
    }
    case 0x4212: { /* HVBJOY */
        uint64_t h = m->cpu.cycles - m->line_start;
        uint8_t v = (uint8_t)(mdr & 0x3Eu);
        if (m->joypad_busy) v |= 0x01u;
        if (h <= 2 || h >= 1096) v |= 0x40u;
        if (m->vcounter >= m->vdisp) v |= 0x80u;
        return v;
    }
    case 0x4213: return m->wrio; /* RDIO: the port pins read back what was written */
    case 0x4214: alu_sync(m); return (uint8_t)m->rddiv;
    case 0x4215: alu_sync(m); return (uint8_t)(m->rddiv >> 8);
    case 0x4216: alu_sync(m); return (uint8_t)m->rdmpy;
    case 0x4217: alu_sync(m); return (uint8_t)(m->rdmpy >> 8);
    case 0x4218: return (uint8_t)m->joy[0];
    case 0x4219: return (uint8_t)(m->joy[0] >> 8);
    case 0x421A: return (uint8_t)m->joy[1];
    case 0x421B: return (uint8_t)(m->joy[1] >> 8);
    case 0x421C: return (uint8_t)m->joy[2];
    case 0x421D: return (uint8_t)(m->joy[2] >> 8);
    case 0x421E: return (uint8_t)m->joy[3];
    case 0x421F: return (uint8_t)(m->joy[3] >> 8);
    default: return mdr; /* write-only registers and unused addresses: open bus */
    }
}

void snes_io_write(SnesMachine *m, uint32_t addr, uint8_t v)
{
    switch (addr & 0xFFFFu) {
    case 0x2180:
        m->wram[m->wram_port] = v;
        snes_wram_written(m, m->wram_port);
        m->wram_port = (m->wram_port + 1u) & 0x1FFFFu;
        return;
    case 0x2181: m->wram_port = (m->wram_port & 0x1FF00u) | v; return;
    case 0x2182: m->wram_port = (m->wram_port & 0x100FFu) | ((uint32_t)v << 8); return;
    case 0x2183: m->wram_port = (m->wram_port & 0x0FFFFu) | ((uint32_t)(v & 1u) << 16); return;
    case 0x4016:
        /* Bit 0 latches both controller ports; while it is high the shift
         * registers keep reloading. */
        m->strobe = v & 1u;
        if (m->strobe) {
            m->shift[0] = m->buttons[0];
            m->shift[1] = m->buttons[1];
        }
        return;
    case 0x4200: { /* NMITIMEN */
        bool nmi_was = (m->nmitimen & 0x80u) != 0;
        m->nmitimen = v;
        /* Enabling NMI during V-blank with the flag still set fires it. */
        if (!nmi_was && (v & 0x80u) && m->rdnmi) {
            m->nmi_transition = true;
            m->nmi_at = m->cpu.cycles;
        }
        if (!(v & 0x30u)) m->timeup = false; /* disabling the timer IRQ drops the line */
        snes_schedule_irq(m);
        snes_update_irq(m);
        return;
    }
    case 0x4201:
        if ((m->wrio & 0x80u) && !(v & 0x80u)) {
            m->ppu.hcounter = snes_hcounter(m);
            m->ppu.vcounter = m->vcounter;
            snes_ppu_latch_counters(&m->ppu);
        }
        m->wrio = v;
        snes_ppu_set_wrio(&m->ppu, v); /* $213F bit 6 reads 1 while WRIO bit 7 is low */
        return;
    case 0x4202: alu_sync(m); m->wrmpya = v; return;
    case 0x4203:
        alu_sync(m);
        m->rdmpy = 0;
        if (m->alu_mul_left || m->alu_div_left) return;
        m->wrmpyb = v;
        m->rddiv = (uint16_t)((v << 8) | m->wrmpya);
        m->alu_mul_left = 8;
        m->alu_shift = v;
        return;
    case 0x4204: alu_sync(m); m->wrdiva = (uint16_t)((m->wrdiva & 0xFF00u) | v); return;
    case 0x4205: alu_sync(m); m->wrdiva = (uint16_t)((m->wrdiva & 0x00FFu) | (v << 8)); return;
    case 0x4206:
        alu_sync(m);
        m->rdmpy = m->wrdiva;
        if (m->alu_mul_left || m->alu_div_left) return;
        m->wrdivb = v;
        m->alu_div_left = 16;
        m->alu_shift = (uint32_t)v << 16;
        return;
    case 0x4207: m->htime = (uint16_t)((m->htime & 0x100u) | v); snes_schedule_irq(m); return;
    case 0x4208: m->htime = (uint16_t)((m->htime & 0x0FFu) | ((v & 1u) << 8)); snes_schedule_irq(m); return;
    case 0x4209: m->vtime = (uint16_t)((m->vtime & 0x100u) | v); snes_schedule_irq(m); return;
    case 0x420A: m->vtime = (uint16_t)((m->vtime & 0x0FFu) | ((v & 1u) << 8)); snes_schedule_irq(m); return;
    case 0x420B:
        for (int n = 0; n < 8; n++) m->dma[n].dma_enable = (v >> n) & 1u;
        if (v) snes_dma_run(m);
        return;
    case 0x420C:
        for (int n = 0; n < 8; n++) m->dma[n].hdma_enable = (v >> n) & 1u;
        return;
    case 0x420D: m->memsel = v & 1u; return;
    default: return;
    }
}

/* Auto-joypad read: starts early in V-blank when NMITIMEN bit 0 is set,
 * latches the controllers, and fills JOY1-4 about 4224 clocks later. */
void snes_joypad_event(SnesMachine *m)
{
    const uint64_t at = m->event_at[SNES_EVENT_JOYPAD];
    snes_schedule(m, SNES_EVENT_JOYPAD, UINT64_MAX);
    if (!m->joypad_busy) {
        if (!(m->nmitimen & 1u)) return;
        m->joypad_busy = true;
        m->joy[0] = m->joy[1] = m->joy[2] = m->joy[3] = 0;
        /* The read clocks the serial ports: latch now (the frame's input). */
        m->shift[0] = m->buttons[0];
        m->shift[1] = m->buttons[1];
        snes_schedule(m, SNES_EVENT_JOYPAD, at + 4224u);
        return;
    }
    m->joypad_busy = false;
    m->joy[0] = m->shift[0];
    m->joy[1] = m->shift[1];
    m->shift[0] = 0xFFFFu; /* the serial registers were shifted out */
    m->shift[1] = 0xFFFFu;
}
