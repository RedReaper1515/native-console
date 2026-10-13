/*
 * I/O registers of both CPUs (GBATEK, "DS I/O Maps").
 *
 * An access of any width at any offset is taken a byte at a time, low byte
 * first, so a register is described once however it is reached. A plain
 * register is a byte of backing store. A register with behaviour acts when
 * the byte that matters arrives: for a 32-bit control register that is its
 * top byte, by which time the rest of the same write is already in place.
 *
 * Devices with a block of registers of their own (the IPC, the card, the
 * maths unit) take whole accesses instead.
 *
 * Unmapped I/O reads as zero and ignores writes.
 */
#include "nds.h"

#include <string.h>

#include "nds_prof.h"

/* IE bits that exist on each CPU. */
static const uint32_t k_ie_mask[2] = { 0x003F3F7Fu, 0x01FF3FFFu };

/* ------------------------------------------------------------------ */
/* Registers both CPUs have                                            */
/* ------------------------------------------------------------------ */

static inline uint8_t byte_of(uint32_t value, uint32_t index)
{
    return (uint8_t)(value >> (index * 8u));
}

/* Registers common to both sides. Returns false if the offset is not one
 * of them. */
static bool common_read(NdsMachine *m, int cpu, uint32_t off, uint8_t *out)
{
    const uint8_t *io = cpu == NDS_ARM9 ? m->io9 : m->io7;

    if (off >= 0x004 && off < 0x006) { /* DISPSTAT */
        *out = byte_of(m->dispstat[cpu], off - 0x004);
        return true;
    }
    if (off >= 0x006 && off < 0x008) { /* VCOUNT */
        *out = byte_of(m->vcount, off - 0x006);
        return true;
    }
    if (off >= 0x0B0 && off < 0x0E0) { /* DMA: all readable on the DS */
        *out = io[off];
        return true;
    }
    if (off >= 0x100 && off < 0x110) { /* timers */
        int index = (int)((off - 0x100) / 4);
        uint32_t byte = (off - 0x100) & 3u;
        if (byte < 2) *out = byte_of(nds_timer_read_counter(m, cpu, index), byte);
        else *out = byte == 2 ? (uint8_t)(m->timer[cpu][index].control & 0xC7u) : 0;
        return true;
    }
    if (off >= 0x130 && off < 0x132) { /* KEYINPUT */
        *out = byte_of(m->keyinput, off - 0x130);
        return true;
    }
    if (off >= 0x132 && off < 0x134) { /* KEYCNT */
        *out = byte_of(m->keycnt[cpu], off - 0x132);
        return true;
    }
    if (off >= 0x208 && off < 0x20C) { /* IME */
        *out = off == 0x208 ? m->ime[cpu] : 0;
        return true;
    }
    if (off >= 0x210 && off < 0x214) { /* IE */
        *out = byte_of(m->ie[cpu], off - 0x210);
        return true;
    }
    if (off >= 0x214 && off < 0x218) { /* IF */
        *out = byte_of(m->if_[cpu], off - 0x214);
        return true;
    }
    return false;
}

static bool common_write(NdsMachine *m, int cpu, uint32_t off, uint8_t value)
{
    uint8_t *io = cpu == NDS_ARM9 ? m->io9 : m->io7;

    if (off == 0x004) { /* DISPSTAT: the three enables and bit 8 of the match line */
        m->dispstat[cpu] = (uint16_t)((m->dispstat[cpu] & 0xFF47u) | (value & 0xB8u));
        return true;
    }
    if (off == 0x005) {
        m->dispstat[cpu] = (uint16_t)((m->dispstat[cpu] & 0x00FFu) | ((uint16_t)value << 8));
        return true;
    }
    if (off == 0x006 || off == 0x007) { /* VCOUNT: taken at the next line */
        uint32_t line = m->vcount_write >= 0 ? (uint32_t)m->vcount_write : m->vcount;
        if (off == 0x006) line = (line & 0x100u) | value;
        else line = (line & 0x0FFu) | ((uint32_t)(value & 1u) << 8);
        m->vcount_write = (int32_t)(line % NDS_LINES_PER_FRAME);
        return true;
    }
    if (off >= 0x0B0 && off < 0x0E0) { /* DMA */
        io[off] = value;
        int ch = (int)((off - 0x0B0) / 12);
        /* The top byte of DMAxCNT holds the enable bit. */
        if ((off - 0x0B0) % 12 == 11) nds_dma_write_control(m, cpu, ch, nds_get_le32(io + 0x0B8 + 12 * ch));
        return true;
    }
    if (off >= 0x100 && off < 0x110) { /* timers */
        int index = (int)((off - 0x100) / 4);
        uint32_t byte = (off - 0x100) & 3u;
        NdsTimer *t = &m->timer[cpu][index];
        if (byte == 0) nds_timer_write_reload(m, cpu, index, (uint16_t)((t->reload & 0xFF00u) | value));
        else if (byte == 1) nds_timer_write_reload(m, cpu, index, (uint16_t)((t->reload & 0x00FFu) | ((uint16_t)value << 8)));
        else if (byte == 2) nds_timer_write_control(m, cpu, index, value & 0xC7u);
        return true;
    }
    if (off >= 0x130 && off < 0x132) return true; /* KEYINPUT is read-only */
    if (off == 0x132) {
        m->keycnt[cpu] = (uint16_t)((m->keycnt[cpu] & 0xFF00u) | value);
        return true;
    }
    if (off == 0x133) {
        m->keycnt[cpu] = (uint16_t)((m->keycnt[cpu] & 0x00FFu) | ((uint16_t)(value & 0xC3u) << 8));
        return true;
    }
    if (off >= 0x208 && off < 0x20C) { /* IME */
        if (off == 0x208) {
            m->ime[cpu] = value & 1u;
            nds_update_irq(m, cpu);
        }
        return true;
    }
    if (off >= 0x210 && off < 0x214) { /* IE */
        uint32_t shift = (off - 0x210) * 8u;
        m->ie[cpu] = ((m->ie[cpu] & ~(0xFFu << shift)) | ((uint32_t)value << shift)) & k_ie_mask[cpu];
        nds_update_irq(m, cpu);
        return true;
    }
    if (off >= 0x214 && off < 0x218) { /* IF: write 1 to acknowledge */
        m->if_[cpu] &= ~((uint32_t)value << ((off - 0x214) * 8u));
        /* The geometry FIFO's request is a level: it stays while its
         * condition holds. */
        if (cpu == NDS_ARM9 && nds_video_gx_irq(m)) m->if_[cpu] |= NDS_IRQ_GXFIFO;
        nds_update_irq(m, cpu);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Debug output                                                        */
/* ------------------------------------------------------------------ */

/* The no$gba debug ports, which homebrew and test programs print through:
 * a character at 0x04FFFA1C, or the address of a zero-terminated string at
 * 0x04FFFA10 or 0x04FFFA14. */
static bool debug_write(NdsMachine *m, int cpu, uint32_t addr, uint32_t value, int size)
{
    if ((addr & 0xFFFFFF00u) != 0x04FFFA00u) return false;
    if (!m->debug_log) return true;
    char text[256];
    if (addr == 0x04FFFA1Cu) {
        text[0] = (char)(value & 0xFFu);
        text[1] = '\0';
        m->debug_log(m, cpu, text);
    } else if ((addr == 0x04FFFA10u || addr == 0x04FFFA14u) && size == 4) {
        size_t length = 0;
        for (; length < sizeof text - 1; length++) {
            uint8_t c = cpu == NDS_ARM9 ? nds9_peek8(m, value + (uint32_t)length) : nds7_peek8(m, value + (uint32_t)length);
            if (!c) break;
            text[length] = (char)c;
        }
        text[length] = '\0';
        m->debug_log(m, cpu, text);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* ARM9                                                                */
/* ------------------------------------------------------------------ */

static uint8_t io9_read8(NdsMachine *m, uint32_t off)
{
    uint8_t value;
    if (common_read(m, NDS_ARM9, off, &value)) return value;

    if (off >= 0x0E0 && off < 0x0F0) return m->io9[off]; /* the DMA fill words */
    if (off >= 0x204 && off < 0x206) return byte_of(m->exmemcnt[NDS_ARM9], off - 0x204);
    if (off == 0x300) return m->postflg[NDS_ARM9];
    if (off >= 0x304 && off < 0x306) return byte_of(m->powcnt1, off - 0x304);
    /* The VRAM and WRAM bank registers read back what was written. GBATEK
     * marks them write-only, but libnds's texture allocator finds its bank
     * by reading them, and the console answers. */
    if (off >= 0x240 && off < 0x24A) {
        if (off == 0x247) return m->wramcnt;
        return m->vramcnt[off < 0x247 ? off - 0x240 : off - 0x241];
    }
    return 0;
}

static void io9_write8(NdsMachine *m, uint32_t off, uint8_t value)
{
    if (common_write(m, NDS_ARM9, off, value)) return;

    if (off >= 0x0E0 && off < 0x0F0) {
        m->io9[off] = value;
        return;
    }
    if (off == 0x204) {
        m->exmemcnt[NDS_ARM9] = (uint16_t)((m->exmemcnt[NDS_ARM9] & 0xFF00u) | value);
        return;
    }
    if (off == 0x205) {
        /* Bits 8-10 and 12 do not exist; bit 13 always reads set. */
        m->exmemcnt[NDS_ARM9] = (uint16_t)((m->exmemcnt[NDS_ARM9] & 0x00FFu) | ((uint16_t)(value & 0xC8u) << 8) | 0x2000u);
        return;
    }
    if (off >= 0x240 && off < 0x24A) {
        if (off == 0x247) {
            m->wramcnt = value & 3u;
            nds_wram_remap(m);
        } else {
            m->vramcnt[off < 0x247 ? off - 0x240 : off - 0x241] = value;
            nds_vram_remap(m);
        }
        return;
    }
    if (off == 0x300) {
        /* Bit 0 can be set and never cleared; bit 1 is plain. */
        m->postflg[NDS_ARM9] = (uint8_t)((m->postflg[NDS_ARM9] & 1u) | (value & 3u));
        return;
    }
    if (off == 0x304 || off == 0x305) {
        /* The 3D engines are brought to this moment before their power
         * changes. */
        const uint16_t before = m->powcnt1;
        nds_video_power_changing(m);
        if (off == 0x304) m->powcnt1 = (uint16_t)((m->powcnt1 & 0xFF00u) | (value & 0x0Fu));
        else m->powcnt1 = (uint16_t)((m->powcnt1 & 0x00FFu) | ((uint16_t)(value & 0x82u) << 8));
        nds_video_power_changed(m, before);
        return;
    }
}

static uint32_t io9_read(NdsMachine *m, uint32_t addr, int size)
{
    if (addr >= 0x04100000u) {
        if ((addr & ~3u) == 0x04100000u) return nds_ipc_recv(m, NDS_ARM9);
        if ((addr & ~3u) == 0x04100010u) return nds_card_data(m, NDS_ARM9);
        return 0;
    }
    uint32_t off = addr - 0x04000000u;
    if (off >= 0x180 && off < 0x18C) return nds_ipc_read(m, NDS_ARM9, off, size);
    if (off >= 0x1A0 && off < 0x1BC) return nds_card_read(m, NDS_ARM9, off, size);
    if (off >= 0x280 && off < 0x2C0) return nds_math_read(m, off, size);

    uint32_t value = 0;
    if (nds_video_read9(m, addr, size, &value)) return value;
    for (int i = 0; i < size; i++) {
        uint32_t at = off + (uint32_t)i;
        if (at < sizeof m->io9) value |= (uint32_t)io9_read8(m, at) << (8 * i);
    }
    return value;
}

static void io9_write(NdsMachine *m, uint32_t addr, uint32_t value, int size)
{
    if (addr >= 0x04100000u) {
        debug_write(m, NDS_ARM9, addr, value, size);
        return;
    }
    uint32_t off = addr - 0x04000000u;
    if (off >= 0x180 && off < 0x18C) {
        nds_ipc_write(m, NDS_ARM9, off, value, size);
        return;
    }
    if (off >= 0x1A0 && off < 0x1BC) {
        nds_card_write(m, NDS_ARM9, off, value, size);
        return;
    }
    if (off >= 0x280 && off < 0x2C0) {
        nds_math_write(m, off, value, size);
        return;
    }
    if (nds_video_write9(m, addr, value, size)) return;
    for (int i = 0; i < size; i++) {
        uint32_t at = off + (uint32_t)i;
        if (at < sizeof m->io9) io9_write8(m, at, (uint8_t)(value >> (8 * i)));
    }
}

/* ------------------------------------------------------------------ */
/* ARM7                                                                */
/* ------------------------------------------------------------------ */

static uint8_t io7_read8(NdsMachine *m, uint32_t off)
{
    uint8_t value;
    if (common_read(m, NDS_ARM7, off, &value)) return value;

    if (off >= 0x136 && off < 0x138) return byte_of(m->extkeyin, off - 0x136);
    if (off == 0x138) return nds_rtc_read(&m->rtc);
    if (off >= 0x204 && off < 0x206) {
        /* EXMEMSTAT: the ARM9's bits 7-15 and our own bits 0-6. */
        uint16_t stat = (uint16_t)((m->exmemcnt[NDS_ARM9] & 0xFF80u) | (m->exmemcnt[NDS_ARM7] & 0x007Fu));
        return byte_of(stat, off - 0x204);
    }
    if (off == 0x240) { /* VRAMSTAT: blocks C and D lent to us */
        uint8_t stat = 0;
        if ((m->vramcnt[2] & 0x87u) == 0x82u) stat |= 1u;
        if ((m->vramcnt[3] & 0x87u) == 0x82u) stat |= 2u;
        return stat;
    }
    if (off == 0x241) return m->wramcnt;
    if (off == 0x300) return m->postflg[NDS_ARM7];
    if (off >= 0x304 && off < 0x306) return byte_of(m->powcnt2, off - 0x304);
    if (off >= 0x308 && off < 0x30C) return byte_of(m->biosprot, off - 0x308);
    /* Serial and the RTC port's unused upper lines: backing store. */
    if ((off >= 0x120 && off < 0x130) || (off >= 0x134 && off < 0x136) || off == 0x139 || off == 0x206 ||
        off == 0x207) {
        return m->io7[off];
    }
    return 0;
}

/* The clock's interrupt output is wired to the serial port's SI input: a
 * falling edge there interrupts the ARM7 if RCNT is in general-purpose mode
 * with the SI interrupt enabled (GBATEK, "DS Real-Time Clock (RTC)"). */
void nds_rtc_line_changed(NdsMachine *m)
{
    /* General-purpose mode is RCNT bits 15-14 = 10; bit 8 enables the SI
     * interrupt. */
    uint16_t rcnt = nds_get_le16(m->io7 + 0x134);
    if (nds_rtc_interrupt(&m->rtc) && (rcnt & 0xC100u) == 0x8100u) nds_raise_irq(m, NDS_ARM7, NDS_IRQ_RTC);
}

static void io7_write8(NdsMachine *m, uint32_t off, uint8_t value)
{
    if (common_write(m, NDS_ARM7, off, value)) return;

    if (off == 0x204) {
        m->exmemcnt[NDS_ARM7] = value & 0x7Fu;
        return;
    }
    if (off == 0x300) {
        m->postflg[NDS_ARM7] |= value & 1u;
        return;
    }
    if (off == 0x301) {
        /* HALTCNT: 0x80 halts until an interrupt; 0xC0 is sleep, which
         * ends the same way here. */
        if (value & 0x80u) m->halted[NDS_ARM7] = true;
        return;
    }
    if (off == 0x304) {
        m->powcnt2 = value & 3u;
        return;
    }
    if (off >= 0x308 && off < 0x30C) {
        uint32_t shift = (off - 0x308) * 8u;
        m->biosprot = (m->biosprot & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        return;
    }
    if (off == 0x138) {
        if (nds_rtc_write(&m->rtc, value, nds_rtc_now(m))) nds_rtc_line_changed(m);
        return;
    }
    if ((off >= 0x120 && off < 0x130) || (off >= 0x134 && off < 0x136) || off == 0x139 || off == 0x206 ||
        off == 0x207) {
        m->io7[off] = value;
        return;
    }
}

static uint32_t io7_read(NdsMachine *m, uint32_t addr, int size)
{
    if (addr >= 0x04100000u) {
        if ((addr & ~3u) == 0x04100000u) return nds_ipc_recv(m, NDS_ARM7);
        if ((addr & ~3u) == 0x04100010u) return nds_card_data(m, NDS_ARM7);
        return 0;
    }
    uint32_t off = addr - 0x04000000u;
    if (off >= 0x180 && off < 0x18C) return nds_ipc_read(m, NDS_ARM7, off, size);
    if (off >= 0x1A0 && off < 0x1BC) return nds_card_read(m, NDS_ARM7, off, size);
    if (off >= 0x1C0 && off < 0x1C4) return nds_spi_read(&m->spi, addr, size);
    if (off >= 0x400 && off < 0x520) return nds_sound_read7(m, addr, size);

    uint32_t value = 0;
    for (int i = 0; i < size; i++) {
        uint32_t at = off + (uint32_t)i;
        if (at < sizeof m->io7) value |= (uint32_t)io7_read8(m, at) << (8 * i);
    }
    return value;
}

static void io7_write(NdsMachine *m, uint32_t addr, uint32_t value, int size)
{
    if (addr >= 0x04100000u) {
        debug_write(m, NDS_ARM7, addr, value, size);
        return;
    }
    uint32_t off = addr - 0x04000000u;
    if (off >= 0x180 && off < 0x18C) {
        nds_ipc_write(m, NDS_ARM7, off, value, size);
        return;
    }
    if (off >= 0x1A0 && off < 0x1BC) {
        nds_card_write(m, NDS_ARM7, off, value, size);
        return;
    }
    if (off >= 0x1C0 && off < 0x1C4) {
        /* The SPI bus: power management, the firmware flash, the touch
         * screen controller. A transfer finishes at once. */
        if (nds_spi_write(&m->spi, m->firmware, addr, value, size)) nds_raise_irq(m, NDS_ARM7, NDS_IRQ_SPI);
        /* What a game writes to the flash (the owner's settings) is for a
         * frontend to keep. */
        if (nds_spi_firmware_dirty(&m->spi)) {
            m->firmware_dirty = true;
            nds_spi_clear_firmware_dirty(&m->spi);
        }
        /* A control byte to the touch controller can switch its pen
         * output, and the power chip can switch the console off. */
        nds_update_extkeyin(m);
        if (nds_spi_power_register(&m->spi, NDS_POWER_REG_CONTROL) & NDS_POWER_SHUTDOWN) m->powered_off = true;
        return;
    }
    if (off >= 0x400 && off < 0x520) {
        nds_sound_write7(m, addr, value, size);
        return;
    }
    for (int i = 0; i < size; i++) {
        uint32_t at = off + (uint32_t)i;
        if (at < sizeof m->io7) io7_write8(m, at, (uint8_t)(value >> (8 * i)));
    }
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

uint32_t nds9_io_read(NdsMachine *m, uint32_t addr, int size)
{
    NDS_PROF_BEGIN(NDS_PROF_IO);
    uint32_t value = io9_read(m, addr, size);
    NDS_PROF_END();
    return value;
}

void nds9_io_write(NdsMachine *m, uint32_t addr, uint32_t value, int size)
{
    NDS_PROF_BEGIN(NDS_PROF_IO);
    io9_write(m, addr, value, size);
    NDS_PROF_END();
}

uint32_t nds7_io_read(NdsMachine *m, uint32_t addr, int size)
{
    NDS_PROF_BEGIN(NDS_PROF_IO);
    uint32_t value = io7_read(m, addr, size);
    NDS_PROF_END();
    return value;
}

void nds7_io_write(NdsMachine *m, uint32_t addr, uint32_t value, int size)
{
    NDS_PROF_BEGIN(NDS_PROF_IO);
    io7_write(m, addr, value, size);
    NDS_PROF_END();
}
