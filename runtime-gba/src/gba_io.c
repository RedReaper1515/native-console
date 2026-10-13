/*
 * I/O registers (0x04000000-0x040003FF).
 *
 * Registers are described as data: for each halfword, how a read behaves
 * (open bus, zero, masked backing store, or computed) and which bits a write
 * stores. Side effects are applied after the store. Read masks follow
 * GBATEK and the mGBA suite's io-read test: write-only registers read as
 * open bus, while the unused upper halves of some 32-bit registers read 0.
 */
#include <string.h>

#include "gba.h"

enum { IO_OPEN = 0, IO_ZERO, IO_MASKED, IO_SPECIAL };

typedef struct {
    uint8_t kind;
    uint16_t read_mask;
    uint16_t write_mask;
} IoReg;

static IoReg g_regs[GBA_IO_SIZE / 2];
static bool g_regs_ready;

static void def(uint32_t off, uint8_t kind, uint16_t read_mask, uint16_t write_mask)
{
    g_regs[off / 2].kind = kind;
    g_regs[off / 2].read_mask = read_mask;
    g_regs[off / 2].write_mask = write_mask;
}

static void build_table(void)
{
    memset(g_regs, 0, sizeof g_regs); /* default: open bus, not writable */

    /* Display */
    def(0x000, IO_MASKED, 0xFFF7, 0xFFF7); /* DISPCNT (bit 3 = CGB mode, fixed 0) */
    def(0x002, IO_MASKED, 0x0001, 0x0001); /* GREENSWAP */
    def(0x004, IO_SPECIAL, 0xFF3F, 0xFF38); /* DISPSTAT */
    def(0x006, IO_SPECIAL, 0x00FF, 0x0000); /* VCOUNT */
    def(0x008, IO_MASKED, 0xDFFF, 0xDFFF);
    def(0x00A, IO_MASKED, 0xDFFF, 0xDFFF);
    def(0x00C, IO_MASKED, 0xFFFF, 0xFFFF);
    def(0x00E, IO_MASKED, 0xFFFF, 0xFFFF);
    for (uint32_t off = 0x010; off < 0x020; off += 2) def(off, IO_OPEN, 0, 0x01FF); /* scroll */
    for (uint32_t off = 0x020; off < 0x040; off += 2) def(off, IO_OPEN, 0, 0xFFFF); /* affine */
    for (uint32_t off = 0x028; off < 0x030; off += 4) def(off + 2, IO_OPEN, 0, 0x0FFF);
    for (uint32_t off = 0x038; off < 0x040; off += 4) def(off + 2, IO_OPEN, 0, 0x0FFF);
    for (uint32_t off = 0x040; off < 0x048; off += 2) def(off, IO_OPEN, 0, 0xFFFF); /* window bounds */
    def(0x048, IO_MASKED, 0x3F3F, 0x3F3F); /* WININ */
    def(0x04A, IO_MASKED, 0x3F3F, 0x3F3F); /* WINOUT */
    def(0x04C, IO_OPEN, 0, 0xFFFF);        /* MOSAIC */
    def(0x050, IO_MASKED, 0x3FFF, 0x3FFF); /* BLDCNT */
    def(0x052, IO_MASKED, 0x1F1F, 0x1F1F); /* BLDALPHA */
    def(0x054, IO_OPEN, 0, 0x001F);        /* BLDY */

    /* Sound */
    def(0x060, IO_MASKED, 0x007F, 0x007F);
    def(0x062, IO_MASKED, 0xFFC0, 0xFFFF);
    def(0x064, IO_MASKED, 0x4000, 0xC7FF);
    def(0x066, IO_ZERO, 0, 0);
    def(0x068, IO_MASKED, 0xFFC0, 0xFFFF);
    def(0x06A, IO_ZERO, 0, 0);
    def(0x06C, IO_MASKED, 0x4000, 0xC7FF);
    def(0x06E, IO_ZERO, 0, 0);
    def(0x070, IO_MASKED, 0x00E0, 0x00E0);
    def(0x072, IO_MASKED, 0xE000, 0xE0FF);
    def(0x074, IO_MASKED, 0x4000, 0xC7FF);
    def(0x076, IO_ZERO, 0, 0);
    def(0x078, IO_MASKED, 0xFF00, 0xFF3F);
    def(0x07A, IO_ZERO, 0, 0);
    def(0x07C, IO_MASKED, 0x40FF, 0xC0FF);
    def(0x07E, IO_ZERO, 0, 0);
    def(0x080, IO_MASKED, 0xFF77, 0xFF77);
    def(0x082, IO_MASKED, 0x770F, 0xFF0F); /* bits 11/15 reset the FIFOs */
    def(0x084, IO_SPECIAL, 0x008F, 0x0080);
    def(0x086, IO_ZERO, 0, 0);
    def(0x088, IO_MASKED, 0xC3FE, 0xC3FE); /* SOUNDBIAS */
    def(0x08A, IO_ZERO, 0, 0);
    for (uint32_t off = 0x090; off < 0x0A0; off += 2) def(off, IO_SPECIAL, 0xFFFF, 0xFFFF); /* wave RAM */
    for (uint32_t off = 0x0A0; off < 0x0A8; off += 2) def(off, IO_OPEN, 0, 0xFFFF);        /* FIFOs */

    /* DMA */
    for (int ch = 0; ch < 4; ch++) {
        uint32_t base = 0x0B0 + 12u * (uint32_t)ch;
        def(base + 0, IO_OPEN, 0, 0xFFFF);
        def(base + 2, IO_OPEN, 0, ch == 0 ? 0x07FF : 0x0FFF);
        def(base + 4, IO_OPEN, 0, 0xFFFF);
        def(base + 6, IO_OPEN, 0, ch == 3 ? 0x0FFF : 0x07FF);
        def(base + 8, IO_ZERO, 0, ch == 3 ? 0xFFFF : 0x3FFF);
        def(base + 10, IO_SPECIAL, ch == 3 ? 0xFFE0 : 0xF7E0, ch == 3 ? 0xFFE0 : 0xF7E0);
    }

    /* Timers */
    for (int t = 0; t < 4; t++) {
        def(0x100 + 4u * (uint32_t)t, IO_SPECIAL, 0xFFFF, 0xFFFF);
        def(0x102 + 4u * (uint32_t)t, IO_SPECIAL, 0x00C7, 0x00C7);
    }

    /* Serial and keypad */
    for (uint32_t off = 0x120; off < 0x12C; off += 2) def(off, IO_MASKED, 0xFFFF, 0xFFFF);
    /* SIOCNT and SIODATA8/SIOMLT_SEND: plain storage without a link cable,
     * the serial port of gba_link.c with one. */
    def(0x128, IO_SPECIAL, 0x7FFF, 0x7FFF);
    def(0x12A, IO_SPECIAL, 0x00FF, 0x00FF);
    def(0x130, IO_SPECIAL, 0x03FF, 0x0000); /* KEYINPUT */
    def(0x132, IO_MASKED, 0xC3FF, 0xC3FF);  /* KEYCNT */
    def(0x134, IO_MASKED, 0xC1FF, 0xC1FF);  /* RCNT */
    def(0x136, IO_ZERO, 0, 0);
    def(0x140, IO_MASKED, 0x0040, 0x0047);  /* JOYCNT */
    def(0x142, IO_ZERO, 0, 0);
    for (uint32_t off = 0x150; off < 0x158; off += 2) def(off, IO_MASKED, 0xFFFF, 0xFFFF);
    def(0x158, IO_MASKED, 0x003A, 0x0030);  /* JOYSTAT */
    def(0x15A, IO_ZERO, 0, 0);

    /* Interrupts, waitstates, power */
    def(0x200, IO_SPECIAL, 0x3FFF, 0x3FFF); /* IE */
    def(0x202, IO_SPECIAL, 0x3FFF, 0x3FFF); /* IF */
    def(0x204, IO_MASKED, 0x5FFF, 0x5FFF);  /* WAITCNT (bit 15: game pak type, reads 0) */
    def(0x206, IO_ZERO, 0, 0);
    def(0x208, IO_SPECIAL, 0x0001, 0x0001); /* IME */
    def(0x20A, IO_ZERO, 0, 0);
    def(0x300, IO_SPECIAL, 0x0001, 0xFF01); /* POSTFLG, HALTCNT (write-only high byte) */
    def(0x302, IO_ZERO, 0, 0);

    g_regs_ready = true;
}

static inline uint16_t backing16(const GbaMachine *m, uint32_t off)
{
    return (uint16_t)(m->io[off] | (m->io[off + 1] << 8));
}

static inline void store16(GbaMachine *m, uint32_t off, uint16_t v)
{
    m->io[off] = (uint8_t)v;
    m->io[off + 1] = (uint8_t)(v >> 8);
}

uint16_t gba_io_read16(GbaMachine *m, uint32_t off)
{
    if (!g_regs_ready) build_table();
    off &= 0x3FEu;
    const IoReg *reg = &g_regs[off / 2];

    switch (reg->kind) {
    case IO_ZERO:
        return 0;
    case IO_MASKED:
        return backing16(m, off) & reg->read_mask;
    case IO_SPECIAL:
        break;
    default: {
        uint32_t bus = gba_open_bus(m);
        return (uint16_t)(bus >> ((off & 2u) * 8u));
    }
    }

    switch (off) {
    case 0x004: return m->dispstat & reg->read_mask;
    case 0x006: return m->vcount;
    case 0x084: return gba_apu_read16(m, off) & reg->read_mask;
    case 0x090: case 0x092: case 0x094: case 0x096:
    case 0x098: case 0x09A: case 0x09C: case 0x09E:
        return gba_apu_read16(m, off);
    case 0x0BA: case 0x0C6: case 0x0D2: case 0x0DE:
        return m->dma[(off - 0x0BA) / 12].control & reg->read_mask;
    case 0x100: case 0x104: case 0x108: case 0x10C:
        return gba_timer_read_counter(m, (int)((off - 0x100) / 4));
    case 0x102: case 0x106: case 0x10A: case 0x10E:
        return m->timer[(off - 0x102) / 4].control & reg->read_mask;
    case 0x128: return m->link ? gba_sio_read_cnt(m) : backing16(m, off) & reg->read_mask;
    case 0x12A: return m->link ? backing16(m, off) : backing16(m, off) & reg->read_mask;
    case 0x130: return m->keyinput;
    case 0x200: return m->ie;
    case 0x202: return m->if_;
    case 0x208: return m->ime;
    case 0x300: return m->io[0x300] & 1u;
    default: return backing16(m, off) & reg->read_mask;
    }
}

static void check_keypad_irq(GbaMachine *m)
{
    uint16_t keycnt = backing16(m, 0x132);
    if (!(keycnt & 0x4000u)) return;
    uint16_t pressed = (uint16_t)(~m->keyinput & 0x3FFu);
    uint16_t want = keycnt & 0x3FFu;
    bool hit = (keycnt & 0x8000u) ? (pressed & want) == want && want : (pressed & want) != 0;
    if (hit) gba_raise_irq(m, GBA_IRQ_KEYPAD);
}

void gba_io_write16(GbaMachine *m, uint32_t off, uint16_t value, uint16_t byte_mask)
{
    if (!g_regs_ready) build_table();
    off &= 0x3FEu;
    const IoReg *reg = &g_regs[off / 2];
    uint16_t mask = reg->write_mask & byte_mask;

    if (off >= 0x090 && off < 0x0A8) {
        gba_apu_write16(m, off, value, byte_mask); /* wave RAM and FIFOs */
        return;
    }

    switch (off) {
    case 0x004:
        m->dispstat = (uint16_t)((m->dispstat & ~mask) | (value & mask));
        return;
    case 0x0BA: case 0x0C6: case 0x0D2: case 0x0DE: {
        int ch = (int)((off - 0x0BA) / 12);
        uint16_t merged = (uint16_t)((m->dma[ch].control & ~byte_mask) | (value & byte_mask));
        gba_dma_write_control(m, ch, merged & reg->write_mask);
        return;
    }
    case 0x100: case 0x104: case 0x108: case 0x10C: {
        int t = (int)((off - 0x100) / 4);
        uint16_t merged = (uint16_t)((m->timer[t].reload & ~byte_mask) | (value & byte_mask));
        gba_timer_write_reload(m, t, merged);
        return;
    }
    case 0x102: case 0x106: case 0x10A: case 0x10E: {
        int t = (int)((off - 0x102) / 4);
        uint16_t merged = (uint16_t)((m->timer[t].control & ~byte_mask) | (value & byte_mask));
        gba_timer_write_control(m, t, merged & reg->write_mask);
        return;
    }
    case 0x128:
    case 0x12A:
        if (m->link) {
            gba_sio_write(m, off, value, byte_mask);
            return;
        }
        break;
    case 0x200:
        m->ie = (uint16_t)((m->ie & ~mask) | (value & mask));
        gba_update_attention(m);
        return;
    case 0x202:
        m->if_ &= (uint16_t)~(value & mask); /* write 1 to acknowledge */
        gba_update_attention(m);
        return;
    case 0x208:
        m->ime = (uint16_t)((m->ime & ~mask) | (value & mask));
        gba_update_attention(m);
        return;
    case 0x300:
        if (byte_mask & 0x00FFu) m->io[0x300] = (uint8_t)((m->io[0x300] & ~1u) | (value & 1u));
        if (byte_mask & 0xFF00u) {
            /* HALTCNT: bit 7 clear = halt, set = stop. */
            if (value & 0x8000u) m->stopped = true;
            else m->halted = true;
            gba_update_attention(m);
        }
        return;
    default:
        break;
    }

    if (off >= 0x060 && off < 0x090 && !(m->io[0x84] & 0x80u) && off < 0x080) return; /* APU off */
    store16(m, off, (uint16_t)((backing16(m, off) & ~mask) | (value & mask)));
    if (off >= 0x060 && off < 0x090) gba_apu_write16(m, off, value & mask, byte_mask);

    switch (off) {
    case 0x028: case 0x02A: case 0x02C: case 0x02E:
    case 0x038: case 0x03A: case 0x03C: case 0x03E:
        gba_ppu_reference_written(m, off);
        break;
    case 0x132:
        check_keypad_irq(m);
        break;
    case 0x204:
        gba_update_waitstates(m);
        break;
    default:
        break;
    }
}

void gba_set_keys(GbaMachine *m, uint16_t pressed_mask)
{
    m->keyinput = (uint16_t)(~pressed_mask & 0x3FFu);
    check_keypad_irq(m);
}
