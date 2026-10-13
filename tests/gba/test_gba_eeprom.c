/*
 * EEPROM save protocol, driven the way games drive it: DMA3 copies request
 * bits from EWRAM to the EEPROM, and a second DMA3 copies the reply back.
 * Checks both address widths (512-byte and 8 KiB parts), that written data
 * reads back, and that it lands in save memory in the byte order the .sav
 * file uses.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba.h"

static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            failures++;                                   \
        }                                                 \
    } while (0)

static void io32(GbaMachine *m, uint32_t off, uint32_t v)
{
    gba_io_write16(m, off, (uint16_t)v, 0xFFFF);
    gba_io_write16(m, off + 2, (uint16_t)(v >> 16), 0xFFFF);
}

/* Run DMA3: 16-bit units, both addresses incrementing, immediate. */
static void dma3(GbaMachine *m, uint32_t src, uint32_t dst, uint16_t count)
{
    io32(m, 0xD4, src);
    io32(m, 0xD8, dst);
    gba_io_write16(m, 0xDC, count, 0xFFFF);
    gba_io_write16(m, 0xDE, 0x8000, 0xFFFF);
    gba_run_cycles(m, 2000);
}

/* Halfwords holding one bit each, as games prepare them in EWRAM. */
static uint32_t put_bits(GbaMachine *m, uint32_t off, uint64_t value, int count)
{
    for (int i = count - 1; i >= 0; i--, off += 2) {
        m->ewram[off] = (uint8_t)((value >> i) & 1u);
        m->ewram[off + 1] = 0;
    }
    return off;
}

static void roundtrip(GbaMachine *m, int width, uint32_t block, uint64_t data)
{
    /* Write request: 10, address, 64 data bits, stop bit. */
    uint32_t off = put_bits(m, 0, 2, 2);
    off = put_bits(m, off, block, width);
    off = put_bits(m, off, data, 64);
    off = put_bits(m, off, 0, 1);
    dma3(m, 0x02000000u, 0x0D000000u, (uint16_t)(off / 2));
    CHECK(m->save_dirty, "write did not mark the save dirty (width %d)", width);
    uint64_t stored = 0;
    for (int i = 0; i < 8; i++) stored = (stored << 8) | m->save[block * 8 + (uint32_t)i];
    CHECK(stored == data, "save memory holds %016llx, wrote %016llx (width %d)", (unsigned long long)stored,
          (unsigned long long)data, width);

    /* Read request: 11, address, stop; then 68 bits back. */
    off = put_bits(m, 0, 3, 2);
    off = put_bits(m, off, block, width);
    off = put_bits(m, off, 0, 1);
    dma3(m, 0x02000000u, 0x0D000000u, (uint16_t)(off / 2));
    dma3(m, 0x0D000000u, 0x02001000u, 68);
    uint64_t got = 0;
    for (int i = 0; i < 68; i++) {
        uint32_t bit = m->ewram[0x1000 + i * 2] & 1u;
        if (i < 4) CHECK(bit == 0, "dummy bit %d is %u (width %d)", i, bit, width);
        else got = (got << 1) | bit;
    }
    CHECK(got == data, "read back %016llx, wrote %016llx (width %d)", (unsigned long long)got,
          (unsigned long long)data, width);
}

int main(void)
{
    static uint8_t rom[0x1000];
    memset(rom, 0, sizeof rom);
    rom[0xB2] = 0x96;
    memcpy(rom + 0x200, "EEPROM_V124", 11);
    rom[0] = 0xFE; rom[1] = 0xFF; rom[2] = 0xFF; rom[3] = 0xEA; /* b . */

    GbaMachine *m = gba_create();
    if (!gba_load_rom(m, rom, sizeof rom)) return 2;
    gba_reset(m, true);
    CHECK(m->save_type == GBA_SAVE_EEPROM, "save type %d, expected EEPROM", (int)m->save_type);

    roundtrip(m, 6, 5, 0x0123456789ABCDEFull);
    roundtrip(m, 14, 0x3FF, 0xFEDCBA9876543210ull);
    roundtrip(m, 14, 0, 0xA5A5A5A55A5A5A5Aull);

    gba_destroy(m);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("eeprom: all checks passed\n");
    return 0;
}
