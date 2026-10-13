/*
 * The RCP's interfaces: MI (interrupts), VI timing (n64_vi.c draws the
 * picture), AI (audio DMA and the DAC), PI (cartridge DMA), RI and the
 * RDRAM registers, SI (PIF DMA) and the CPU's view of the SP and DP.
 *
 * Register behaviour, DMA timing and the PI's block quirks follow ares
 * (n64/mi, vi, ai, pi, si; ISC). Ported from ares
 * (https://github.com/ares-emulator/ares). Copyright (c) 2004-2025 ares
 * team, Near et al. Permission to use, copy, modify, and/or distribute this
 * software for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear in all
 * copies.
 */
#include "n64_io.h"

#include <string.h>

#define VI_CLOCK_NTSC 48681812u
#define VI_CLOCK_PAL 49656530u

static uint32_t vi_clock(const N64Machine *m) { return m->cart.pal ? VI_CLOCK_PAL : VI_CLOCK_NTSC; }

/* ---- MI ---- */

uint32_t n64_mi_read(N64Machine *m, uint32_t offset)
{
    switch ((offset >> 2) & 3u) {
    case 0: return m->mi_mode;
    case 1: return 0x02020102u; /* RSP, RDP, RAC, IO revisions */
    case 2:
        n64_sync_rsp(m);
        return m->mi_intr;
    default: return m->mi_mask;
    }
}

void n64_mi_write(N64Machine *m, uint32_t offset, uint32_t v)
{
    switch ((offset >> 2) & 3u) {
    case 0:
        m->mi_mode = (m->mi_mode & ~0x7Fu) | (v & 0x7Fu);
        if (v & 0x0080u) m->mi_mode &= ~0x80u;
        if (v & 0x0100u) m->mi_mode |= 0x80u;
        if (v & 0x0200u) m->mi_mode &= ~0x100u;
        if (v & 0x0400u) m->mi_mode |= 0x100u;
        if (v & 0x0800u) n64_mi_lower(m, N64_MI_DP);
        if (v & 0x1000u) m->mi_mode &= ~0x200u;
        if (v & 0x2000u) m->mi_mode |= 0x200u;
        n64_rdram_update_fast(m); /* repeat mode bypasses the CPU's RDRAM shortcut */
        break;
    case 3:
        for (unsigned b = 0; b < 6; b++) {
            if (v & (1u << (2 * b))) m->mi_mask &= ~(1u << b);
            if (v & (2u << (2 * b))) m->mi_mask |= 1u << b;
        }
        n64_mi_raise(m, 0); /* re-evaluate the CPU line */
        break;
    default: break;
    }
}

/* ---- VI timing ---- */

enum { VI_CONTROL, VI_ORIGIN, VI_WIDTH, VI_V_INTR, VI_V_CURRENT, VI_BURST, VI_V_SYNC, VI_H_SYNC, VI_LEAP, VI_H_START,
       VI_V_START, VI_V_BURST, VI_X_SCALE, VI_Y_SCALE };

uint32_t n64_vi_read(N64Machine *m, uint32_t offset)
{
    unsigned r = (offset >> 2) & 15u;
    if (r == VI_V_CURRENT) return ((m->vi_line & 0x1FFu) << 1) | (m->vi_field & 1u);
    return r < 14 ? m->vi_regs[r] : 0;
}

void n64_vi_write(N64Machine *m, uint32_t offset, uint32_t v)
{
    unsigned r = (offset >> 2) & 15u;
    static const uint32_t masks[14] = { 0x0001FFFFu, 0x00FFFFFFu, 0x00000FFFu, 0x000003FFu, 0, 0x3FFFFFFFu,
                                        0x000003FFu, 0x001F0FFFu, 0x0FFF0FFFu, 0x03FF03FFu, 0x03FF03FFu,
                                        0x03FF03FFu, 0x0FFF0FFFu, 0x0FFF0FFFu };
    if (r >= 14) return;
    if (r == VI_V_CURRENT) {
        n64_mi_lower(m, N64_MI_VI);
        return;
    }
    m->vi_regs[r] = v & masks[r];
}

static bool vi_active(const N64Machine *m) { return (m->vi_regs[VI_CONTROL] & 3u) != 0; }

/* CPU cycles in one scanline: H_SYNC + 1 VI clocks, the fraction carried. */
uint64_t n64_vi_line_cycles(N64Machine *m)
{
    uint32_t h = m->vi_regs[VI_H_SYNC] & 0xFFFu;
    uint64_t vi_clocks = h ? h + 1u : 3094u;
    uint64_t num = vi_clocks * N64_CPU_HZ + m->vi_frac;
    uint32_t clk = vi_clock(m);
    m->vi_frac = (uint32_t)(num % clk);
    return num / clk;
}

void n64_vi_line(N64Machine *m, uint64_t at)
{
    uint32_t vsync = m->vi_regs[VI_V_SYNC] & 0x3FFu;
    if (!vsync) vsync = m->cart.pal ? 625u : 525u;
    const bool progressive = (vsync & 1u) != 0;
    m->vi_line = (m->vi_line + 1u) & 0x1FFu;
    uint32_t halfline = (m->vi_line << 1) | m->vi_field;
    if (halfline >= vsync + 1u) {
        m->vi_line = 0;
        if (!progressive) m->vi_field ^= 1u;
        /* A field is complete: scan it out. */
        if (!m->video_off) {
            const uint64_t t0 = m->perf_on ? n64_host_ticks() : 0;
            n64_vi_render(m);
            if (m->perf_on) m->perf_vi += n64_host_ticks() - t0;
        }
        m->frame_count++;
        m->frame_ready = true;
    }
    if (vi_active(m)) {
        const uint32_t vintr = m->vi_regs[VI_V_INTR] & 0x3FFu;
        if (progressive) {
            if (m->vi_line == vintr >> 1) n64_mi_raise(m, N64_MI_VI);
        } else if (vintr & 1u) {
            if (m->vi_line == vintr >> 1) n64_mi_raise(m, N64_MI_VI);
        } else {
            halfline = (m->vi_line << 1) | m->vi_field;
            if (!m->vi_field && halfline == vintr) n64_mi_raise(m, N64_MI_VI);
            if (m->vi_field && halfline + 1u == vintr) n64_mi_raise(m, N64_MI_VI);
            if (!m->vi_field && halfline == vsync && vintr == 0) n64_mi_raise(m, N64_MI_VI);
        }
    }
    n64_schedule(m, N64_EV_VI, at + n64_vi_line_cycles(m));
}

/* ---- AI ---- */

uint32_t n64_ai_read(N64Machine *m, uint32_t offset)
{
    unsigned r = (offset >> 2) & 7u;
    if (r != 3) return m->ai_length[0] & 0x3FFFFu;
    uint32_t v = 0x01100000u; /* bits 20 and 24 always read set */
    if (m->ai_count > 1) v |= 0x80000001u;
    if (m->ai_count > 0) v |= 0x40000000u;
    if (m->ai_enable) v |= 0x02000000u;
    return v;
}

void n64_ai_write(N64Machine *m, uint32_t offset, uint32_t v)
{
    switch ((offset >> 2) & 7u) {
    case 0:
        if (m->ai_count < 2) m->ai_dram_addr[m->ai_count] = v & 0xFFFFF8u;
        break;
    case 1:
        if (m->ai_count < 2) {
            if (m->ai_count == 0) n64_mi_raise(m, N64_MI_AI);
            m->ai_length[m->ai_count] = v & 0x3FFF8u;
            m->ai_count++;
        }
        break;
    case 2: m->ai_enable = (v & 1u) != 0; break;
    case 3: n64_mi_lower(m, N64_MI_AI); break;
    case 4: m->ai_dacrate = v & 0x3FFFu; break;
    case 5: m->ai_bitrate = v & 0xFu; break;
    default: break;
    }
}

/* CPU cycles per DAC sample: (DACRATE + 1) VI clocks; 44.1 kHz before a
 * game sets the rate. */
uint64_t n64_ai_period(N64Machine *m)
{
    uint64_t num, den;
    if (m->ai_dacrate) {
        num = (uint64_t)N64_CPU_HZ * (m->ai_dacrate + 1u);
        den = vi_clock(m);
    } else {
        num = N64_CPU_HZ;
        den = 44100u;
    }
    num += m->ai_frac;
    m->ai_frac = (uint32_t)(num % den);
    uint64_t p = num / den;
    return p ? p : 1u;
}

static void audio_push(N64Machine *m, int16_t l, int16_t r)
{
    uint32_t next = (m->audio_head + 1u) % N64_AUDIO_RING;
    if (next == m->audio_tail) m->audio_tail = (m->audio_tail + 1u) % N64_AUDIO_RING; /* drop the oldest */
    m->audio_ring[2 * m->audio_head] = l;
    m->audio_ring[2 * m->audio_head + 1] = r;
    m->audio_head = next;
}

void n64_ai_sample(N64Machine *m, uint64_t at)
{
    const int16_t prev_l = m->ai_left, prev_r = m->ai_right;
    bool active = false;
    if (m->ai_count && m->ai_length[0] && m->ai_enable) {
        uint32_t a = m->ai_dram_addr[0];
        uint32_t data = a + 4u <= m->rdram_size ? n64_be32(m->rdram + a) : 0;
        m->ai_left = (int16_t)(uint16_t)(data >> 16);
        m->ai_right = (int16_t)(uint16_t)data;
        /* The address counter carries out of bit 12 one sample late. */
        uint32_t low = (a & 0x1FFFu) + 4u;
        a = (a & ~0x1FFFu) | (low & 0x1FFFu);
        if ((low & 0x1FFFu) == 0) a += 0x2000u;
        m->ai_dram_addr[0] = a & 0xFFFFFFu;
        m->ai_length[0] -= 4u;
        active = true;
    }
    if (m->ai_count && m->ai_length[0] == 0) {
        if (--m->ai_count) {
            m->ai_dram_addr[0] = m->ai_dram_addr[1];
            m->ai_length[0] = m->ai_length[1];
            n64_mi_raise(m, N64_MI_AI);
        }
    }
    if (!active) {
        /* The DAC's output decays to silence rather than holding an offset. */
        m->ai_left = (int16_t)((m->ai_left * 255) / 256);
        m->ai_right = (int16_t)((m->ai_right * 255) / 256);
    }
    const uint64_t period = n64_ai_period(m);
    if (!m->audio_off) {
        /* Linear interpolation from the previous sample to this one, onto
         * the fixed output rate. Positions are in CPU cycles x rate. */
        const uint64_t total = period * N64_AUDIO_RATE;
        uint64_t pos = m->ai_phase;
        while (pos < total) {
            int64_t l = prev_l + ((int64_t)(m->ai_left - prev_l) * (int64_t)pos) / (int64_t)total;
            int64_t r = prev_r + ((int64_t)(m->ai_right - prev_r) * (int64_t)pos) / (int64_t)total;
            audio_push(m, (int16_t)l, (int16_t)r);
            pos += N64_CPU_HZ;
        }
        m->ai_phase = pos - total;
    }
    n64_schedule(m, N64_EV_AI, at + period);
}

/* ---- PI ---- */

/* Which domain's timing a cartridge address uses: 1 for the ROM and the
 * 64DD IPL, 2 for the 64DD registers and SRAM/FlashRAM. */
static const uint32_t *pi_domain(const N64Machine *m, uint32_t addr)
{
    if ((addr >= 0x05000000u && addr < 0x06000000u) || (addr >= 0x08000000u && addr < 0x10000000u))
        return &m->pi_bsd[4];
    return &m->pi_bsd[0];
}

static uint64_t pi_duration(N64Machine *m, uint32_t len)
{
    const uint32_t *bsd = pi_domain(m, m->pi_cart_addr);
    const uint32_t lat = bsd[0] & 0xFFu, pwd = bsd[1] & 0xFFu, pgs = bsd[2] & 0xFu, rls = bsd[3] & 3u;
    len = (len | 1u) + 1u;
    uint32_t shift = pgs + 2u, page = 1u << shift, mask = page - 1u;
    uint32_t first = m->pi_cart_addr, last = m->pi_cart_addr + len - 2u;
    uint32_t pages = (last >> shift) - (first >> shift) + 1u;
    uint32_t buffers = 0, partial = 0;
    if ((first >> shift) == (last >> shift)) {
        if (len == 128) buffers = 1;
        else partial = len;
    } else {
        if ((first & mask) == 0) buffers++;
        else partial += page - (first & mask);
        if (((last + 2u) & mask) == 0) buffers++;
        else partial += (last & mask) + 2u;
        if (pages > 2) buffers += (pages - 2u) * page / 128u;
    }
    uint64_t cycles = (uint64_t)(14u + lat + 1u) * pages + (uint64_t)(pwd + 1u + rls + 1u) * len / 2u + buffers * 28u +
                      partial;
    /* PI clocks at 62.5 MHz: 3/2 CPU cycles each. */
    return cycles * 3u / 2u;
}

/* RDRAM -> cartridge (PI_RD_LEN). */
static void pi_dma_to_cart(N64Machine *m)
{
    uint32_t len = (m->pi_rd_len | 1u) + 1u;
    const uint32_t *bsd = pi_domain(m, m->pi_cart_addr);
    uint32_t page_mask = (1u << ((bsd[2] & 0xFu) + 2u)) - 1u;
    n64_cart_select(m, m->pi_cart_addr);
    for (uint32_t i = 0; i < len; i += 2) {
        uint32_t cursor = m->pi_cart_addr + i;
        if (i && (cursor & page_mask) == 0) n64_cart_select(m, cursor);
        uint32_t a = (m->pi_dram_addr + i) & 0xFFFFFFu;
        uint16_t v = a + 2u <= m->rdram_size ? n64_be16(m->rdram + a) : 0;
        n64_cart_write_half(m, v);
    }
}

/* Cartridge -> RDRAM (PI_WR_LEN), with the hardware's block structure: the
 * first block of a misaligned transfer and short transfers land
 * byte-exact; the rest in halfwords from an 8-byte aligned RDRAM address. */
static void pi_dma_to_ram(N64Machine *m)
{
    uint8_t mem[128];
    int32_t length = (int32_t)(m->pi_wr_len & 0xFFFFFFu) + 1;
    int32_t max_block = 128;
    bool first = true, selected = false;
    const uint32_t *bsd = pi_domain(m, m->pi_cart_addr);
    uint32_t page_mask = (1u << ((bsd[2] & 0xFu) + 2u)) - 1u;
    uint32_t dram = m->pi_dram_addr & 0xFFFFFFu;
    const uint32_t start = dram;

    while (length > 0) {
        int32_t misalign = (int32_t)(dram & 7u);
        int32_t row_left = 0x800 - (int32_t)(dram & 0x7FFu);
        int32_t block = max_block - misalign < row_left ? max_block - misalign : row_left;
        int32_t cur = length < block ? length : block;
        for (int32_t i = 0; i < cur; i += 2) {
            if (!selected || (m->pi_cart_addr & page_mask) == 0) {
                n64_cart_select(m, m->pi_cart_addr);
                selected = true;
            }
            uint16_t v = n64_cart_read_half(m);
            if (i + 1 < 128) {
                mem[i] = (uint8_t)(v >> 8);
                mem[i + 1] = (uint8_t)v;
            }
            m->pi_cart_addr += 2;
            length -= 2;
        }
        if (first && cur < 127 - misalign) {
            for (int32_t i = 0; i < cur - misalign; i++) {
                if (dram < m->rdram_size) m->rdram[dram] = mem[i];
                dram++;
            }
        } else {
            for (int32_t i = 0; i < cur - misalign; i += 2) {
                if (dram < m->rdram_size) m->rdram[dram] = mem[i];
                dram++;
                if (dram < m->rdram_size) m->rdram[dram] = mem[i + 1];
                dram++;
            }
        }
        dram = (dram + 7u) & ~7u;
        m->pi_wr_len = cur <= 8 ? (uint32_t)(127 - misalign) : 127u;
        first = false;
        max_block = row_left < 8 ? 128 - misalign : 128;
    }
    if (dram > start) n64_ram_written(m, start, dram - start);
    m->pi_dram_addr = dram;
}

uint32_t n64_pi_read(N64Machine *m, uint32_t offset)
{
    unsigned r = (offset >> 2) & 15u;
    switch (r) {
    case 0: return m->pi_dram_addr;
    case 1: return m->pi_cart_addr;
    case 2: return m->pi_rd_len;
    case 3: return m->pi_wr_len;
    case 4: return m->pi_status;
    case 13: case 14: return m->pi_latch;
    default: return r >= 5 && r <= 12 ? m->pi_bsd[r - 5] : 0;
    }
}

void n64_pi_write(N64Machine *m, uint32_t offset, uint32_t v)
{
    unsigned r = (offset >> 2) & 15u;
    if (r != 4 && (m->pi_status & 3u)) {
        m->pi_status |= 4u; /* written while busy: error */
        return;
    }
    switch (r) {
    case 0: m->pi_dram_addr = v & 0xFFFFFEu; break;
    case 1: m->pi_cart_addr = v & ~1u; break;
    case 2:
        m->pi_rd_len = v & 0xFFFFFFu;
        m->pi_status |= 1u;
        n64_schedule(m, N64_EV_PI, m->cpu.cycles + pi_duration(m, m->pi_rd_len));
        pi_dma_to_cart(m);
        break;
    case 3:
        m->pi_wr_len = v & 0xFFFFFFu;
        m->pi_status |= 1u;
        n64_schedule(m, N64_EV_PI, m->cpu.cycles + pi_duration(m, m->pi_wr_len));
        pi_dma_to_ram(m);
        break;
    case 4:
        if (v & 1u) {
            m->pi_status &= ~5u;
            n64_schedule(m, N64_EV_PI, UINT64_MAX);
        }
        if (v & 2u) {
            m->pi_status &= ~8u;
            n64_mi_lower(m, N64_MI_PI);
        }
        break;
    default:
        if (r >= 5 && r <= 12) {
            static const uint32_t masks[4] = { 0xFFu, 0xFFu, 0xFu, 3u };
            m->pi_bsd[r - 5] = v & masks[(r - 5) & 3u];
        }
        break;
    }
}

void n64_pi_dma_done(N64Machine *m)
{
    m->pi_status = (m->pi_status & ~1u) | 8u;
    n64_mi_raise(m, N64_MI_PI);
}

/* ---- SI ---- */

uint32_t n64_si_read(N64Machine *m, uint32_t offset)
{
    switch ((offset >> 2) & 7u) {
    case 0: return m->si_dram_addr;
    case 6: return m->si_status;
    default: return 0;
    }
}

void n64_si_write(N64Machine *m, uint32_t offset, uint32_t v)
{
    switch ((offset >> 2) & 7u) {
    case 0: m->si_dram_addr = v & 0xFFFFF8u; break;
    case 1: /* PIF RAM -> RDRAM */
        m->si_status |= 1u;
        m->si_pending = 1;
        n64_schedule(m, N64_EV_SI, m->cpu.cycles + n64_pif_read_cycles(m));
        break;
    case 4: /* RDRAM -> PIF RAM */
        m->si_status |= 1u;
        m->si_pending = 2;
        n64_schedule(m, N64_EV_SI, m->cpu.cycles + 4065u * 3u / 2u);
        break;
    case 6:
        m->si_status &= ~0x1000u;
        n64_mi_lower(m, N64_MI_SI);
        break;
    default: break;
    }
}

void n64_si_dma_done(N64Machine *m)
{
    if (m->si_pending == 1) n64_pif_dma_read(m);
    else if (m->si_pending == 2) n64_pif_dma_write(m);
    m->si_pending = 0;
    m->si_status = (m->si_status & ~1u) | 0x1000u;
    n64_mi_raise(m, N64_MI_SI);
}

/* ---- the SP and DP as the CPU sees them ---- */

uint32_t n64_sp_read(N64Machine *m, uint32_t paddr)
{
    n64_sync_rsp(m);
    if (paddr < 0x04040000u) {
        const uint8_t *mem = (paddr & 0x1000u) ? m->rsp.imem : m->rsp.dmem;
        return n64_be32(mem + (paddr & 0xFFCu));
    }
    if (paddr < 0x04080000u) return n64_rsp_reg_read(&m->rsp, (paddr >> 2) & 7u);
    if (paddr == 0x04080000u) return n64_rsp_reg_read(&m->rsp, N64_SP_REG_PC);
    if (paddr >= 0x04100000u && paddr < 0x04200000u) return n64_rdp_reg_read(&m->rdp, (paddr >> 2) & 7u);
    return 0;
}

void n64_sp_write(N64Machine *m, uint32_t paddr, uint32_t v)
{
    n64_sync_rsp(m);
    if (paddr < 0x04040000u) {
        uint8_t *mem = (paddr & 0x1000u) ? m->rsp.imem : m->rsp.dmem;
        n64_put_be32(mem + (paddr & 0xFFCu), v);
        return;
    }
    if (paddr < 0x04080000u) {
        const bool was_halted = n64_rsp_halted(&m->rsp);
        n64_rsp_reg_write(&m->rsp, (paddr >> 2) & 7u, v);
        if (was_halted && !n64_rsp_halted(&m->rsp)) n64_rsp_started(m);
        return;
    }
    if (paddr == 0x04080000u) {
        n64_rsp_reg_write(&m->rsp, N64_SP_REG_PC, v);
        return;
    }
    if (paddr >= 0x04100000u && paddr < 0x04200000u) {
        const uint64_t t0 = m->perf_on ? n64_host_ticks() : 0;
        n64_rdp_reg_write(&m->rdp, (paddr >> 2) & 7u, v);
        if (m->perf_on) m->perf_rdp += n64_host_ticks() - t0;
    }
}
