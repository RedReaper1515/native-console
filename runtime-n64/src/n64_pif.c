/*
 * The PIF's side of the serial interface: its 64 bytes of RAM, moved in and
 * out by SI DMA, and the joybus commands they carry to the controllers
 * (ports 0-3) and the cartridge's EEPROM (port 4).
 *
 * The command walk, the controller and Controller Pak replies, the data
 * CRC and the EEPROM commands follow ares (n64/pif/hle.cpp,
 * n64/controller/gamepad/gamepad.cpp, n64/cartridge/joybus.cpp; ISC).
 * Ported from ares (https://github.com/ares-emulator/ares). Copyright (c)
 * 2004-2025 ares team, Near et al. Permission to use, copy, modify, and/or
 * distribute this software for any purpose with or without fee is hereby
 * granted, provided that the above copyright notice and this permission
 * notice appear in all copies.
 *
 * The boot handshake with IPL3 (ROM lockout, the checksum exchange,
 * "terminate boot") follows ares's PIF::mainHLE, and the CIC-NUS-6105
 * challenge ares's CIC::challenge (n64/pif/hle.cpp, n64/cic/commands.cpp;
 * ISC). The checksum IPL3 computes is accepted without comparing it with
 * the CIC's. When IPL3 does not run (n64_boot.c), the PIF starts in its
 * running state and boot commands are acknowledged and ignored.
 */
#include "n64_io.h"

#include <string.h>

uint8_t n64_pif_data_crc(const uint8_t *data)
{
    uint8_t crc = 0;
    for (int i = 0; i < 33; i++) {
        for (int j = 7; j >= 0; j--) {
            uint8_t x = (crc & 0x80u) ? 0x85u : 0x00u;
            crc = (uint8_t)(crc << 1);
            if (i < 32 && (data[i] & (1u << j))) crc |= 1u;
            crc ^= x;
        }
    }
    return crc;
}

/* A controller's reply. Returns 1 when the device answered. */
static int controller(N64Machine *m, int port, uint32_t send, uint32_t recv, const uint8_t *in, uint8_t *out)
{
    if (!(m->controllers & (1u << port))) return 0;
    const bool pak = port == 0 && m->cart.cpak;
    switch (in[0]) {
    case 0x00:
    case 0xFF:
        if (recv < 3) return 0;
        out[0] = 0x05;
        out[1] = 0x00;
        out[2] = pak ? 0x01 : 0x02;
        return 1;
    case 0x01:
        if (recv < 4) return 0;
        out[0] = (uint8_t)(m->buttons[port] >> 8);
        out[1] = (uint8_t)m->buttons[port];
        out[2] = (uint8_t)m->stick_x[port];
        out[3] = (uint8_t)m->stick_y[port];
        return 1;
    case 0x02: { /* Controller Pak read: 32 bytes and their CRC */
        if (send < 3 || recv < 1) return 0;
        uint32_t addr = ((uint32_t)in[1] << 8 | in[2]) & ~0x1Fu;
        memset(out, 0, recv < 33 ? recv : 33);
        if (pak) {
            for (uint32_t i = 0; i < 32 && i < recv; i++) out[i] = addr + i < 0x8000u ? m->mempak[port][addr + i] : 0;
        }
        if (recv >= 33) {
            out[32] = n64_pif_data_crc(out);
            if (!pak) out[32] ^= 0xFFu;
        }
        return 1;
    }
    case 0x03: { /* Controller Pak write */
        if (send < 4 || recv < 1) return 0;
        uint32_t addr = ((uint32_t)in[1] << 8 | in[2]) & ~0x1Fu;
        uint32_t len = send - 3u;
        if (pak && addr < 0x8000u) {
            for (uint32_t i = 0; i < len && addr + i < 0x8000u; i++) m->mempak[port][addr + i] = in[3 + i];
            m->save_dirty = true;
        }
        out[0] = len == 32 ? n64_pif_data_crc(in + 3) : 0;
        if (!pak) out[0] ^= 0xFFu;
        return 1;
    }
    default: return 0;
    }
}

static int cartridge(N64Machine *m, uint32_t send, uint32_t recv, const uint8_t *in, uint8_t *out)
{
    const uint32_t size = m->cart.eeprom_kbit == 16 ? 2048u : (m->cart.eeprom_kbit == 4 ? 512u : 0u);
    if (!size) return 0;
    switch (in[0]) {
    case 0x00:
    case 0xFF:
        if (recv < 3) return 0;
        out[0] = 0x00;
        out[1] = size == 2048u ? 0xC0 : 0x80;
        out[2] = m->eeprom_busy ? 0x80 : 0x00;
        return 1;
    case 0x04: {
        if (send < 2) return 0;
        uint32_t addr = (uint32_t)in[1] * 8u;
        for (uint32_t i = 0; i < recv; i++) out[i] = m->eeprom_busy ? 0xFF : m->eeprom[(addr + i) & (size - 1u)];
        return 1;
    }
    case 0x05: {
        if (send < 2 || recv < 1) return 0;
        out[0] = m->eeprom_busy ? 0x80 : 0x00;
        if (!m->eeprom_busy) {
            uint32_t addr = (uint32_t)in[1] * 8u;
            for (uint32_t i = 0; i + 2 < send; i++) m->eeprom[(addr + i) & (size - 1u)] = in[2 + i];
            m->eeprom_busy = true;
            m->save_dirty = true;
            n64_schedule(m, N64_EV_EEPROM, m->cpu.cycles + 93750u * 12u); /* 6 ms at 187.5 MHz in ares */
        }
        return 1;
    }
    default: return 0;
    }
}

/* Run the command list in PIF RAM: each channel's block is a send length,
 * a receive length, the command bytes and room for the reply. */
static void joybus_run(N64Machine *m)
{
    uint8_t *ram = m->pif_ram;
    uint32_t offset = 0;
    int channel = 0;
    while (channel < 5 && offset < 63u) {
        uint8_t send = ram[offset++];
        if (send == 0xFE) break;
        if (send == 0xFF) continue;
        if (send == 0x00) {
            channel++;
            continue;
        }
        if (send == 0xFD) {
            channel++;
            continue;
        }
        if (offset >= 64u) break;
        const uint32_t recv_at = offset;
        uint8_t recv = ram[offset++];
        if (recv == 0xFE) break;
        const uint32_t s = send & 0x3Fu, r = recv & 0x3Fu;
        if (offset + s + r > 64u) break;
        uint8_t in[64], out[64];
        memcpy(in, ram + offset, s);
        offset += s;
        memset(out, 0, sizeof out);
        int valid = s ? (channel < 4 ? controller(m, channel, s, r, in, out) : cartridge(m, s, r, in, out)) : 0;
        if (!valid) {
            ram[recv_at] = (uint8_t)(0x80u | r);
        } else {
            memcpy(ram + offset, out, r);
        }
        offset += r;
        channel++;
    }
}

uint64_t n64_pif_read_cycles(N64Machine *m)
{
    uint64_t cycles = 13600;
    uint32_t short_cmds = 0, offset = 0;
    int channel = 0;
    while (offset < 64u && channel < 5) {
        uint8_t send = m->pif_ram[offset++];
        if (send == 0xFE) {
            short_cmds++;
            break;
        }
        if (send == 0x00 || send == 0xFD) {
            short_cmds++;
            channel++;
            continue;
        }
        if (send == 0xFF) {
            short_cmds++;
            continue;
        }
        if (offset >= 64u) break;
        uint8_t recv = m->pif_ram[offset++];
        offset += (send & 0x3Fu) + (recv & 0x3Fu);
        cycles += channel < 4 ? ((m->controllers & (1u << channel)) ? 22000u : 18000u) : 20000u;
        channel++;
    }
    cycles += 1420u * short_cmds;
    /* PIF clocks to CPU cycles (ares counts these at 187.5 MHz, x3). */
    return cycles * 3u / 2u;
}

/* CIC-NUS-6105 (and 7105): the anti-piracy challenge, 30 nibbles at PIF
 * RAM 0x30 answered in place. Other CICs invert the nibbles. */
static void cic_challenge(N64Machine *m)
{
    uint8_t mem[30];
    for (unsigned i = 0; i < 15; i++) {
        mem[2 * i] = m->pif_ram[0x30 + i] >> 4;
        mem[2 * i + 1] = m->pif_ram[0x30 + i] & 15u;
    }
    if (m->cart.cic == 6105) {
        static const uint8_t lut[32] = { 0x4, 0x7, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1, 0xC, 0xF, 0x8, 0xF, 0x6, 0x3, 0x6, 0x9,
                                         0x4, 0x1, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1, 0xC, 0x9, 0x8, 0x5, 0x6, 0x3, 0xC, 0x9 };
        uint8_t key = 0xB, sel = 0;
        for (unsigned i = 0; i < 30; i++) {
            const uint8_t data = (uint8_t)((key + 5u * mem[i]) & 15u);
            mem[i] = data;
            key = lut[sel << 4 | data];
            uint8_t mod = data >> 3;
            uint8_t mag = data & 7u;
            if (mod) mag = (uint8_t)(~mag & 7u);
            if (mag % 3u != 1u) mod = !mod;
            if (sel) {
                if (data == 0x1 || data == 0x9) mod = 1;
                if (data == 0xB || data == 0xE) mod = 0;
            }
            sel = mod;
        }
    } else {
        for (unsigned i = 0; i < 30; i++) mem[i] = (uint8_t)(~mem[i] & 15u);
    }
    for (unsigned i = 0; i < 15; i++) m->pif_ram[0x30 + i] = (uint8_t)(mem[2 * i] << 4 | mem[2 * i + 1]);
}

/* The PIF's boot handshake, run after every write to its RAM. While IPL2
 * runs, the CIC's info byte and seeds are visible at 0x25-0x27; the
 * checksum command hides them, and hides the checksum IPL3 left at
 * 0x32-0x37. */
static void boot_commands(N64Machine *m)
{
    for (;;) {
        const uint8_t cmd = m->pif_ram[63];
        switch (m->pif_state) {
        case N64_PIF_WAIT_LOCKOUT:
            if (!(cmd & 0x10u)) return;
            m->pif_rom_lockout = true;
            m->pif_state = N64_PIF_WAIT_GET_CHECKSUM;
            break;
        case N64_PIF_WAIT_GET_CHECKSUM:
            if (!(cmd & 0x20u)) return;
            memset(m->pif_ram + 0x25, 0, 3);
            memset(m->pif_ram + 0x32, 0, 6);
            m->pif_ram[63] |= 0x80u;
            m->pif_state = N64_PIF_WAIT_CHECK_CHECKSUM;
            break;
        case N64_PIF_WAIT_CHECK_CHECKSUM:
            if (!(cmd & 0x40u)) return;
            m->pif_state = N64_PIF_WAIT_TERMINATE;
            break;
        case N64_PIF_WAIT_TERMINATE:
            if (!(cmd & 0x08u)) return;
            m->pif_ram[63] = 0;
            m->pif_state = N64_PIF_RUN;
            return;
        default:
            /* Running: boot commands are acknowledged and ignored. */
            if (cmd & 0x08u) m->pif_ram[63] &= (uint8_t)~0x08u;
            if (cmd & 0x10u) m->pif_ram[63] &= (uint8_t)~0x10u;
            if (cmd & 0x20u) m->pif_ram[63] = (uint8_t)((m->pif_ram[63] & ~0x20u) | 0x80u);
            if (cmd & 0x40u) m->pif_ram[63] &= (uint8_t)~0x40u;
            return;
        }
    }
}

void n64_pif_cpu_written(N64Machine *m)
{
    if (m->pif_ram[63] & 0x01u) {
        m->pif_ram[63] &= (uint8_t)~0x01u;
        m->pif_joy_armed = true;
    }
    boot_commands(m);
}

void n64_pif_dma_read(N64Machine *m)
{
    if (m->pif_ram[63] & 0x02u) cic_challenge(m);
    else if (m->pif_joy_armed) joybus_run(m);
    uint32_t a = m->si_dram_addr;
    for (uint32_t i = 0; i < 64u; i++) {
        if (a + i < m->rdram_size) m->rdram[a + i] = m->pif_ram[i];
    }
    n64_ram_written(m, a, 64);
}

void n64_pif_dma_write(N64Machine *m)
{
    uint32_t a = m->si_dram_addr;
    for (uint32_t i = 0; i < 64u; i++) m->pif_ram[i] = a + i < m->rdram_size ? m->rdram[a + i] : 0;
    /* "Run the joybus" (bit 0): the PIF clears the bit and executes the
     * list on the next read. */
    n64_pif_cpu_written(m);
}
