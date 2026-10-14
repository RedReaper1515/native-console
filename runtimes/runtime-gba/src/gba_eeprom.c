/*
 * EEPROM saves (512 bytes or 8 KiB), accessed bit-serially through bit 0
 * of halfword accesses to the top of the cartridge space, normally by DMA3
 * (GBATEK "GBA Cart Backup EEPROM").
 *
 * Requests arrive as a whole DMA: "11" + address + "0" asks to read the
 * 64-bit block at address; "10" + address + 64 data bits + "0" writes one.
 * The address width (6 bits for 512 bytes, 14 for 8 KiB) follows from the
 * request length, so requests are decoded when their DMA completes. A read
 * then returns 4 dummy bits and the 64 data bits, most significant first.
 */
#include "gba.h"

void gba_eeprom_write_bit(GbaMachine *m, uint32_t bit)
{
    GbaEeprom *e = &m->eeprom;
    if (e->in_count < sizeof e->in) e->in[e->in_count++] = (uint8_t)(bit & 1u);
}

uint32_t gba_eeprom_read_bit(GbaMachine *m)
{
    GbaEeprom *e = &m->eeprom;
    if (e->out_pos < 68) return e->out[e->out_pos++];
    return 1; /* ready */
}

static uint32_t bits_value(const uint8_t *bits, uint32_t count)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < count; i++) v = (v << 1) | bits[i];
    return v;
}

void gba_eeprom_request_done(GbaMachine *m)
{
    GbaEeprom *e = &m->eeprom;
    uint32_t n = e->in_count;
    e->in_count = 0;
    if (n < 3) return;

    if (e->in[0] == 1 && e->in[1] == 1 && (n == 9 || n == 17)) {
        uint32_t width = n - 3;
        uint32_t block = bits_value(&e->in[2], width) & (width == 6 ? 0x3Fu : 0x3FFu);
        e->width = width;
        for (int i = 0; i < 4; i++) e->out[i] = 0;
        for (int i = 0; i < 64; i++) {
            uint8_t byte = m->save[block * 8u + (uint32_t)(i / 8)];
            e->out[4 + i] = (uint8_t)((byte >> (7 - (i % 8))) & 1u);
        }
        e->out_pos = 0;
    } else if (e->in[0] == 1 && e->in[1] == 0 && (n == 73 || n == 81)) {
        uint32_t width = n - 67;
        uint32_t block = bits_value(&e->in[2], width) & (width == 6 ? 0x3Fu : 0x3FFu);
        e->width = width;
        for (int i = 0; i < 8; i++) {
            m->save[block * 8u + (uint32_t)i] = (uint8_t)bits_value(&e->in[2 + width + (uint32_t)i * 8u], 8);
        }
        m->save_dirty = true;
        e->out_pos = 68; /* reads now report ready */
    }
}

bool gba_eeprom_address(const GbaMachine *m, uint32_t addr)
{
    if (m->save_type != GBA_SAVE_EEPROM || (addr >> 24) != 0x0Du) return false;
    /* Carts larger than 16 MiB keep ROM in the low half of 0x0D000000. */
    return m->rom_size <= 0x1000000u || addr >= 0x0DFFFF00u;
}
