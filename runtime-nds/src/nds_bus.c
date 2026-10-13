/*
 * The two memory maps (GBATEK, "DS Memory Maps" and "DS Memory Control").
 *
 * ARM9: ITCM from 0, DTCM wherever CP15 puts it, main RAM at 0x02000000,
 * its share of the shared WRAM at 0x03000000, I/O at 0x04000000, palettes
 * at 0x05000000, VRAM at 0x06000000, OAM at 0x07000000, the GBA slot at
 * 0x08000000 and its BIOS at 0xFFFF0000.
 *
 * ARM7: its BIOS at 0, main RAM, the shared WRAM and its own 64 KiB at
 * 0x03000000/0x03800000, I/O, Wi-Fi at 0x04800000, and any VRAM lent to it
 * at 0x06000000.
 *
 * Each region is mirrored through the 16 MiB it sits in. Unmapped memory
 * reads as zero. The CPU cores pass addresses as issued; halfword and word
 * accesses are aligned here.
 */
#include "nds.h"

#include <string.h>

_Static_assert(offsetof(NdsMachine, arm9) == 0, "the ARM9 must be the first member of NdsMachine");

/* ------------------------------------------------------------------ */
/* Derived maps                                                        */
/* ------------------------------------------------------------------ */

static void map_pages(uint8_t **table, uint32_t table_pages, uint32_t first, uint8_t *block, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes / 0x4000u; i++) {
        if (first + i < table_pages) table[first + i] = block + i * 0x4000u;
    }
}

/* VRAMCNT_A-I to page tables (GBATEK, "DS Memory Control - VRAM"). Blocks
 * are applied in order A to I; where two claim the same page the later one
 * is what the CPU and the display see. */
void nds_vram_remap(NdsMachine *m)
{
    static const uint32_t block_base[9] = { NDS_VRAM_A, NDS_VRAM_B, NDS_VRAM_C, NDS_VRAM_D, NDS_VRAM_E,
                                            NDS_VRAM_F, NDS_VRAM_G, NDS_VRAM_H, NDS_VRAM_I };
    static const uint32_t block_size[9] = { 0x20000, 0x20000, 0x20000, 0x20000, 0x10000,
                                            0x4000, 0x4000, 0x8000, 0x4000 };
    NdsVramMap *map = &m->map;
    memset(map, 0, sizeof *map);

    for (int block = 0; block < 9; block++) {
        uint8_t cnt = m->vramcnt[block];
        if (!(cnt & 0x80u)) continue;
        uint8_t *mem = m->vram + block_base[block];
        uint32_t size = block_size[block];
        uint32_t ofs = (cnt >> 3) & 3u;
        /* Bit 2 of MST does not exist on blocks A, B, H and I. */
        uint32_t mst = cnt & ((block == 0 || block == 1 || block >= 7) ? 3u : 7u);

        if (mst == 0) {
            map_pages(map->lcdc, 64, block_base[block] / 0x4000u, mem, size);
            continue;
        }
        switch (block) {
        case 0: /* A */
        case 1: /* B */
            if (mst == 1) map_pages(map->bg_a, 32, ofs * 8u, mem, size);
            else if (mst == 2) map_pages(map->obj_a, 16, (ofs & 1u) * 8u, mem, size);
            else map->texture[ofs] = mem;
            break;
        case 2: /* C */
        case 3: /* D */
            if (mst == 1) map_pages(map->bg_a, 32, ofs * 8u, mem, size);
            else if (mst == 2) map_pages(map->arm7, 16, (ofs & 1u) * 8u, mem, size);
            else if (mst == 3) map->texture[ofs] = mem;
            else if (mst == 4 && block == 2) map_pages(map->bg_b, 8, 0, mem, size);
            else if (mst == 4) map_pages(map->obj_b, 8, 0, mem, size);
            break;
        case 4: /* E */
            if (mst == 1) map_pages(map->bg_a, 32, 0, mem, size);
            else if (mst == 2) map_pages(map->obj_a, 16, 0, mem, size);
            else if (mst == 3) {
                for (int slot = 0; slot < 4; slot++) map->tex_pal[slot] = mem + slot * 0x4000;
            } else if (mst == 4) {
                for (int slot = 0; slot < 4; slot++) map->bg_ext_a[slot] = mem + slot * 0x2000;
            }
            break;
        case 5: /* F */
        case 6: /* G */ {
            /* 16 KiB at 0, 0x4000, 0x10000 or 0x14000, and again 0x8000 above. */
            uint32_t page = (ofs & 1u) + ((ofs >> 1) & 1u) * 4u;
            if (mst == 1) {
                map_pages(map->bg_a, 32, page, mem, size);
                map_pages(map->bg_a, 32, page + 2u, mem, size);
            } else if (mst == 2) {
                map_pages(map->obj_a, 16, page, mem, size);
                map_pages(map->obj_a, 16, page + 2u, mem, size);
            } else if (mst == 3) {
                map->tex_pal[(ofs & 1u) + ((ofs >> 1) & 1u) * 4u] = mem;
            } else if (mst == 4) {
                map->bg_ext_a[(ofs & 1u) * 2u] = mem;
                map->bg_ext_a[(ofs & 1u) * 2u + 1u] = mem + 0x2000;
            } else if (mst == 5) {
                map->obj_ext_a = mem;
            }
            break;
        }
        case 7: /* H: 32 KiB, repeated 0x10000 above */
            if (mst == 1) {
                map_pages(map->bg_b, 8, 0, mem, size);
                map_pages(map->bg_b, 8, 4, mem, size);
            } else if (mst == 2) {
                for (int slot = 0; slot < 4; slot++) map->bg_ext_b[slot] = mem + slot * 0x2000;
            }
            break;
        default: /* I: 16 KiB */
            if (mst == 1) {
                /* At 0x8000, filling the half H leaves, in both copies. */
                map->bg_b[2] = map->bg_b[3] = mem;
                map->bg_b[6] = map->bg_b[7] = mem;
            } else if (mst == 2) {
                for (int page = 0; page < 8; page++) map->obj_b[page] = mem;
            } else if (mst == 3) {
                map->obj_ext_b = mem;
            }
            break;
        }
    }
}

/* WRAMCNT: who gets which half of the 32 KiB (GBATEK, "DS Memory Control -
 * WRAM"). */
void nds_wram_remap(NdsMachine *m)
{
    const uint8_t *wram9_before = m->wram9, *wram7_before = m->wram7;
    const uint32_t mask9_before = m->wram9_mask, mask7_before = m->wram7_mask;

    switch (m->wramcnt & 3u) {
    case 0:
        m->wram9 = m->shared_wram;
        m->wram9_mask = 0x7FFFu;
        m->wram7 = NULL;
        m->wram7_mask = 0;
        break;
    case 1:
        m->wram9 = m->shared_wram + 0x4000;
        m->wram9_mask = 0x3FFFu;
        m->wram7 = m->shared_wram;
        m->wram7_mask = 0x3FFFu;
        break;
    case 2:
        m->wram9 = m->shared_wram;
        m->wram9_mask = 0x3FFFu;
        m->wram7 = m->shared_wram + 0x4000;
        m->wram7_mask = 0x3FFFu;
        break;
    default:
        m->wram9 = NULL;
        m->wram9_mask = 0;
        m->wram7 = m->shared_wram;
        m->wram7_mask = 0x7FFFu;
        break;
    }

    /* Recompiled code in the 0x03000000 area has to be checked again. */
    if (m->wram9 != wram9_before || m->wram7 != wram7_before || m->wram9_mask != mask9_before ||
        m->wram7_mask != mask7_before) {
        m->rc_map_seq = ++m->rc_seq;
    }
}

/* The TCM regions from CP15: ITCM always starts at 0 and is mirrored
 * through its virtual size; DTCM has a base and a size. Each region's size
 * is 512 bytes shifted left by the register's size field, at least 4 KiB. */
void nds_tcm_remap(NdsMachine *m)
{
    const NdsCp15 *cp = &m->arm9.cp15;
    const uint32_t itcm_end_before = m->itcm_end;
    const bool itcm_load_before = m->itcm_load;

    m->itcm_end = 0;
    m->itcm_load = (cp->control & NDS_CP15_ITCM_LOAD_MODE) != 0;
    if (cp->control & NDS_CP15_ITCM_ENABLE) {
        uint32_t n = (cp->itcm >> 1) & 0x1Fu;
        if (n < 3) n = 3;
        m->itcm_end = n >= 23 ? 0xFFFFFFFFu : (0x200u << n);
    }

    m->dtcm_base = 0;
    m->dtcm_size = 0;
    m->dtcm_load = (cp->control & NDS_CP15_DTCM_LOAD_MODE) != 0;
    if (cp->control & NDS_CP15_DTCM_ENABLE) {
        uint32_t n = (cp->dtcm >> 1) & 0x1Fu;
        if (n < 3) n = 3;
        m->dtcm_size = n >= 23 ? 0xFFFFFFFFu : (0x200u << n);
        m->dtcm_base = cp->dtcm & 0xFFFFF000u & ~(m->dtcm_size - 1u);
    }

    /* Where opcodes come from changed (the DTCM never supplies them):
     * recompiled code has to be checked against memory again. */
    if (m->itcm_end != itcm_end_before || m->itcm_load != itcm_load_before) m->rc_map_seq = ++m->rc_seq;
}

void nds9_bus_cp15_changed(NdsCpu *cpu)
{
    nds_tcm_remap(NDS9_MACHINE(cpu));
}

void nds9_bus_wait_for_interrupt(NdsCpu *cpu)
{
    NDS9_MACHINE(cpu)->halted[NDS_ARM9] = true;
}

/* ------------------------------------------------------------------ */
/* ARM9                                                                */
/* ------------------------------------------------------------------ */

static inline uint8_t *vram9_page(NdsMachine *m, uint32_t addr)
{
    switch ((addr >> 21) & 7u) {
    case 0: return m->map.bg_a[(addr >> 14) & 31u];
    case 1: return m->map.bg_b[(addr >> 14) & 7u];
    case 2: return m->map.obj_a[(addr >> 14) & 15u];
    case 3: return m->map.obj_b[(addr >> 14) & 7u];
    default: return m->map.lcdc[(addr >> 14) & 63u];
    }
}

/* The GBA slot with nothing in it: the ROM area reads as the address
 * divided by two for the CPU the slot is assigned to, zero for the other
 * (GBATEK, "DS Memory Control - Cartridges and Main RAM"). */
static uint32_t gba_slot_read(const NdsMachine *m, int cpu, uint32_t addr, int size)
{
    bool mine = ((m->exmemcnt[NDS_ARM9] >> 7) & 1u) == (uint32_t)cpu;
    if (!mine) return 0;
    if (addr >= 0x0A000000u) return size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
    uint32_t low = (addr >> 1) & 0xFFFFu;
    if (size == 4) return low | ((((addr >> 1) + 1u) & 0xFFFFu) << 16);
    if (size == 2) return low;
    return (low >> ((addr & 1u) * 8u)) & 0xFFu;
}

/* A read outside the TCMs. side_effects is false for peeks. */
static inline uint32_t read9(NdsMachine *m, uint32_t addr, int size, bool side_effects)
{
    switch (addr >> 24) {
    case 0x02:
        return nds_get_le(m->main_ram + (addr & (NDS_MAIN_RAM_SIZE - 1u)), size);
    case 0x03:
        return m->wram9 ? nds_get_le(m->wram9 + (addr & m->wram9_mask), size) : 0;
    case 0x04:
        return side_effects ? nds9_io_read(m, addr, size) : 0;
    case 0x05:
        return nds_get_le(m->palette + (addr & (NDS_PALETTE_SIZE - 1u)), size);
    case 0x06: {
        const uint8_t *page = vram9_page(m, addr);
        return page ? nds_get_le(page + (addr & 0x3FFFu), size) : 0;
    }
    case 0x07:
        return nds_get_le(m->oam + (addr & (NDS_OAM_SIZE - 1u)), size);
    case 0x08:
    case 0x09:
    case 0x0A:
        return gba_slot_read(m, NDS_ARM9, addr, size);
    case 0xFF:
        if ((addr & 0xFFFF0000u) == 0xFFFF0000u && (addr & 0xFFFFu) < NDS_BIOS9_SIZE) {
            return nds_get_le(m->bios9 + (addr & (NDS_BIOS9_SIZE - 1u)), size);
        }
        return 0;
    default:
        return 0;
    }
}

static inline void write9(NdsMachine *m, uint32_t addr, uint32_t value, int size)
{
    switch (addr >> 24) {
    case 0x02:
        nds_put_le(m->main_ram + (addr & (NDS_MAIN_RAM_SIZE - 1u)), value, size);
        nds_rc_note_write(m, NDS_RC_MAIN + (addr & (NDS_MAIN_RAM_SIZE - 1u)));
        break;
    case 0x03:
        if (m->wram9) {
            uint8_t *at = m->wram9 + (addr & m->wram9_mask);
            nds_put_le(at, value, size);
            nds_rc_note_write(m, NDS_RC_SHARED + (uint32_t)(at - m->shared_wram));
        }
        break;
    case 0x04:
        nds9_io_write(m, addr, value, size);
        break;
    /* Palettes, VRAM and OAM ignore byte writes. */
    case 0x05:
        if (size != 1) nds_put_le(m->palette + (addr & (NDS_PALETTE_SIZE - 1u)), value, size);
        break;
    case 0x06: {
        uint8_t *page = vram9_page(m, addr);
        if (page && size != 1) nds_put_le(page + (addr & 0x3FFFu), value, size);
        break;
    }
    case 0x07:
        if (size != 1) nds_put_le(m->oam + (addr & (NDS_OAM_SIZE - 1u)), value, size);
        break;
    default:
        break;
    }
}

/* Cycles for one ARM9 access to memory outside the TCMs. */
static inline uint32_t cycles9(uint32_t addr, int access)
{
    if ((addr >> 24) == 0x02u) return (access & NDS_ACCESS_CODE) ? NDS9_CYCLES_CODE : NDS9_CYCLES_DATA;
    return NDS9_CYCLES_BUS;
}

/* The TCMs sit between the CPU and the bus. ITCM answers opcode fetches and
 * data accesses; DTCM answers data accesses only. In load mode a TCM takes
 * writes but reads go on to the bus. DMA never sees them. */
static inline uint32_t bus9_read(NdsCpu *cpu, uint32_t addr, int size, int access)
{
    NdsMachine *m = NDS9_MACHINE(cpu);
    addr &= ~((uint32_t)size - 1u);
    if (!(access & NDS_ACCESS_DMA)) {
        if (addr < m->itcm_end && !m->itcm_load) {
            cpu->cycles += NDS9_CYCLES_TCM;
            return nds_get_le(m->itcm + (addr & (NDS_ITCM_SIZE - 1u)), size);
        }
        if (!(access & NDS_ACCESS_CODE) && addr - m->dtcm_base < m->dtcm_size && !m->dtcm_load) {
            cpu->cycles += NDS9_CYCLES_TCM;
            return nds_get_le(m->dtcm + (addr & (NDS_DTCM_SIZE - 1u)), size);
        }
    }
    cpu->cycles += (access & NDS_ACCESS_CODE) ? nds9_code_cycles(m, addr) : cycles9(addr, access);
    return read9(m, addr, size, true);
}

static inline void bus9_write(NdsCpu *cpu, uint32_t addr, uint32_t value, int size, int access)
{
    NdsMachine *m = NDS9_MACHINE(cpu);
    addr &= ~((uint32_t)size - 1u);
    if (!(access & NDS_ACCESS_DMA)) {
        if (addr < m->itcm_end) {
            cpu->cycles += NDS9_CYCLES_TCM;
            nds_put_le(m->itcm + (addr & (NDS_ITCM_SIZE - 1u)), value, size);
            nds_rc_note_write(m, NDS_RC_ITCM + (addr & (NDS_ITCM_SIZE - 1u)));
            return;
        }
        if (addr - m->dtcm_base < m->dtcm_size) {
            cpu->cycles += NDS9_CYCLES_TCM;
            nds_put_le(m->dtcm + (addr & (NDS_DTCM_SIZE - 1u)), value, size);
            return;
        }
    }
    cpu->cycles += cycles9(addr, access);
    write9(m, addr, value, size);
}

uint32_t nds9_bus_read8(NdsCpu *cpu, uint32_t addr, int access)
{
    return bus9_read(cpu, addr, 1, access);
}

uint32_t nds9_bus_read16(NdsCpu *cpu, uint32_t addr, int access)
{
    return bus9_read(cpu, addr, 2, access);
}

uint32_t nds9_bus_read32(NdsCpu *cpu, uint32_t addr, int access)
{
    return bus9_read(cpu, addr, 4, access);
}

void nds9_bus_write8(NdsCpu *cpu, uint32_t addr, uint8_t value, int access)
{
    bus9_write(cpu, addr, value, 1, access);
}

void nds9_bus_write16(NdsCpu *cpu, uint32_t addr, uint16_t value, int access)
{
    bus9_write(cpu, addr, value, 2, access);
}

void nds9_bus_write32(NdsCpu *cpu, uint32_t addr, uint32_t value, int access)
{
    bus9_write(cpu, addr, value, 4, access);
}

void nds9_bus_idle(NdsCpu *cpu, int cycles)
{
    cpu->cycles += (uint64_t)cycles;
}

static uint32_t peek9(NdsMachine *m, uint32_t addr, int size)
{
    addr &= ~((uint32_t)size - 1u);
    if (addr < m->itcm_end && !m->itcm_load) return nds_get_le(m->itcm + (addr & (NDS_ITCM_SIZE - 1u)), size);
    if (addr - m->dtcm_base < m->dtcm_size && !m->dtcm_load) {
        return nds_get_le(m->dtcm + (addr & (NDS_DTCM_SIZE - 1u)), size);
    }
    return read9(m, addr, size, false);
}

uint32_t nds9_peek32(NdsMachine *m, uint32_t addr)
{
    return peek9(m, addr, 4);
}

uint32_t nds9_peek_code(NdsMachine *m, uint32_t addr, int size)
{
    addr &= ~((uint32_t)size - 1u);
    if (addr < m->itcm_end && !m->itcm_load) return nds_get_le(m->itcm + (addr & (NDS_ITCM_SIZE - 1u)), size);
    return read9(m, addr, size, false);
}

uint8_t nds9_peek8(NdsMachine *m, uint32_t addr)
{
    return (uint8_t)peek9(m, addr, 1);
}

/* ------------------------------------------------------------------ */
/* ARM7                                                                */
/* ------------------------------------------------------------------ */

static inline uint32_t read7(NdsMachine *m, uint32_t addr, int size, bool side_effects)
{
    switch (addr >> 24) {
    case 0x00:
        if (addr < NDS_BIOS7_SIZE) {
            /* Code outside the BIOS cannot read its protected part. */
            if (side_effects && addr < m->biosprot && m->arm7.r[15] >= NDS_BIOS7_SIZE) {
                return size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
            }
            return nds_get_le(m->bios7 + addr, size);
        }
        return 0;
    case 0x02:
        return nds_get_le(m->main_ram + (addr & (NDS_MAIN_RAM_SIZE - 1u)), size);
    case 0x03:
        if (!(addr & 0x00800000u) && m->wram7) return nds_get_le(m->wram7 + (addr & m->wram7_mask), size);
        return nds_get_le(m->arm7_wram + (addr & (NDS_ARM7_WRAM_SIZE - 1u)), size);
    case 0x04:
        if (addr >= 0x04800000u && addr < 0x04810000u) {
            return nds_get_le(m->wifi + (addr & 0x7FFFu & ~((uint32_t)size - 1u)), size);
        }
        return side_effects ? nds7_io_read(m, addr, size) : 0;
    case 0x06: {
        const uint8_t *page = m->map.arm7[(addr >> 14) & 15u];
        return page ? nds_get_le(page + (addr & 0x3FFFu), size) : 0;
    }
    case 0x08:
    case 0x09:
    case 0x0A:
        return gba_slot_read(m, NDS_ARM7, addr, size);
    default:
        return 0;
    }
}

static inline void write7(NdsMachine *m, uint32_t addr, uint32_t value, int size)
{
    switch (addr >> 24) {
    case 0x02:
        nds_put_le(m->main_ram + (addr & (NDS_MAIN_RAM_SIZE - 1u)), value, size);
        nds_rc_note_write(m, NDS_RC_MAIN + (addr & (NDS_MAIN_RAM_SIZE - 1u)));
        break;
    case 0x03:
        if (!(addr & 0x00800000u) && m->wram7) {
            uint8_t *at = m->wram7 + (addr & m->wram7_mask);
            nds_put_le(at, value, size);
            nds_rc_note_write(m, NDS_RC_SHARED + (uint32_t)(at - m->shared_wram));
        } else {
            nds_put_le(m->arm7_wram + (addr & (NDS_ARM7_WRAM_SIZE - 1u)), value, size);
            nds_rc_note_write(m, NDS_RC_ARM7_WRAM + (addr & (NDS_ARM7_WRAM_SIZE - 1u)));
        }
        break;
    case 0x04:
        if (addr >= 0x04800000u && addr < 0x04810000u) {
            nds_put_le(m->wifi + (addr & 0x7FFFu & ~((uint32_t)size - 1u)), value, size);
        } else {
            nds7_io_write(m, addr, value, size);
        }
        break;
    case 0x06: {
        /* VRAM lent to the ARM7 is plain work RAM: byte writes work. */
        uint8_t *page = m->map.arm7[(addr >> 14) & 15u];
        if (page) nds_put_le(page + (addr & 0x3FFFu), value, size);
        break;
    }
    default:
        break;
    }
}

static inline uint32_t bus7_read(NdsCpu *cpu, uint32_t addr, int size)
{
    cpu->cycles += NDS7_CYCLES_ACCESS;
    return read7(NDS7_MACHINE(cpu), addr & ~((uint32_t)size - 1u), size, true);
}

static inline void bus7_write(NdsCpu *cpu, uint32_t addr, uint32_t value, int size)
{
    cpu->cycles += NDS7_CYCLES_ACCESS;
    write7(NDS7_MACHINE(cpu), addr & ~((uint32_t)size - 1u), value, size);
}

uint32_t nds7_bus_read8(NdsCpu *cpu, uint32_t addr, int access)
{
    (void)access;
    return bus7_read(cpu, addr, 1);
}

uint32_t nds7_bus_read16(NdsCpu *cpu, uint32_t addr, int access)
{
    (void)access;
    return bus7_read(cpu, addr, 2);
}

uint32_t nds7_bus_read32(NdsCpu *cpu, uint32_t addr, int access)
{
    (void)access;
    return bus7_read(cpu, addr, 4);
}

void nds7_bus_write8(NdsCpu *cpu, uint32_t addr, uint8_t value, int access)
{
    (void)access;
    bus7_write(cpu, addr, value, 1);
}

void nds7_bus_write16(NdsCpu *cpu, uint32_t addr, uint16_t value, int access)
{
    (void)access;
    bus7_write(cpu, addr, value, 2);
}

void nds7_bus_write32(NdsCpu *cpu, uint32_t addr, uint32_t value, int access)
{
    (void)access;
    bus7_write(cpu, addr, value, 4);
}

void nds7_bus_idle(NdsCpu *cpu, int cycles)
{
    cpu->cycles += (uint64_t)cycles;
}

uint32_t nds7_device_read32(NdsMachine *m, uint32_t addr)
{
    uint32_t region = addr >> 24;
    return region == 0x02u || region == 0x03u ? read7(m, addr & ~3u, 4, false) : 0;
}

void nds7_device_write32(NdsMachine *m, uint32_t addr, uint32_t value)
{
    uint32_t region = addr >> 24;
    if (region == 0x02u || region == 0x03u) write7(m, addr & ~3u, value, 4);
}

uint32_t nds7_peek_code(NdsMachine *m, uint32_t addr, int size)
{
    return read7(m, addr & ~((uint32_t)size - 1u), size, false);
}

uint32_t nds7_peek32(NdsMachine *m, uint32_t addr)
{
    return read7(m, addr & ~3u, 4, false);
}

uint8_t nds7_peek8(NdsMachine *m, uint32_t addr)
{
    return (uint8_t)read7(m, addr, 1, false);
}
