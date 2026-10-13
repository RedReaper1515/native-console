/*
 * GBA memory map, access timing and open bus.
 *
 * Every CPU access lands here through the gba_bus_* functions declared in
 * gba_bus.h. Addresses arrive as the CPU issued them; bus_align() drops the
 * low bits for every region except the 8-bit save bus. Region numbers are
 * address bits 24-27; anything at 0x10000000 and above is unmapped.
 */
#include <string.h>

#include "gba.h"
#include "gba_bus.h"

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t vram_offset(uint32_t addr)
{
    uint32_t off = addr & 0x1FFFFu;
    return off >= GBA_VRAM_SIZE ? off - 0x8000u : off;
}

/* ------------------------------------------------------------------ */
/* Waitstates                                                          */
/* ------------------------------------------------------------------ */

void gba_update_waitstates(GbaMachine *m)
{
    static const uint8_t n_table[4] = { 4, 3, 2, 8 };
    static const uint8_t s_table[3][2] = { { 2, 1 }, { 4, 1 }, { 8, 1 } };
    uint16_t waitcnt = (uint16_t)(m->io[0x204] | (m->io[0x205] << 8));

    for (int r = 0; r < 16; r++) {
        m->wait_n16[r] = m->wait_s16[r] = m->wait_n32[r] = m->wait_s32[r] = 1;
    }

    /* EWRAM: 16-bit bus, waitstates from the internal memory control. */
    uint32_t ew_wait = 15u - ((m->memcnt >> 24) & 15u);
    m->wait_n16[2] = m->wait_s16[2] = (uint8_t)(1 + ew_wait);
    m->wait_n32[2] = m->wait_s32[2] = (uint8_t)(2 * (1 + ew_wait));

    /* Palette and VRAM have 16-bit buses. */
    m->wait_n32[5] = m->wait_s32[5] = 2;
    m->wait_n32[6] = m->wait_s32[6] = 2;

    for (int ws = 0; ws < 3; ws++) {
        uint32_t n_bits = (waitcnt >> (2 + ws * 3)) & 3u;
        uint32_t s_bit = (waitcnt >> (4 + ws * 3)) & 1u;
        uint8_t n = (uint8_t)(1 + n_table[n_bits]);
        uint8_t s = (uint8_t)(1 + s_table[ws][s_bit]);
        for (int half = 0; half < 2; half++) {
            int r = 8 + ws * 2 + half;
            m->wait_n16[r] = n;
            m->wait_s16[r] = s;
            m->wait_n32[r] = (uint8_t)(n + s);
            m->wait_s32[r] = (uint8_t)(2 * s);
        }
    }

    uint8_t sram = (uint8_t)(1 + n_table[waitcnt & 3u]);
    for (int r = 14; r < 16; r++) {
        m->wait_n16[r] = m->wait_s16[r] = m->wait_n32[r] = m->wait_s32[r] = sram;
    }

    /* The prefetch buffer starts empty when it is switched on (and is not
     * used while it is off). */
    bool pf = (waitcnt & 0x4000u) != 0;
    if (pf != m->pf_enabled) {
        m->pf_enabled = pf;
        m->pf_check = m->pf_read = 0;
        m->pf_count = m->pf_fetch = 0;
        m->pf_full = m->pf_following = m->pf_boundary = m->pf_force_nseq = false;
        m->pf_at = m->cpu.cycles;
    }
}

/* ------------------------------------------------------------------ */
/* Game Pak prefetch buffer                                            */
/* ------------------------------------------------------------------ */

/* While the cartridge bus is idle, the prefetcher reads the halfwords after
 * the last opcode fetched from ROM into an 8-halfword buffer (GBATEK "Game
 * Pak ROM Prefetch"). The cycle-level rules are the ones the Alyosha
 * prefetcher tests pin down, as GBAHawk (by the tests' author) implements
 * them:
 * - a read takes the sequential waitstates and starts in the cycle after
 *   the previous one; none runs during the CPU's own cartridge access, or
 *   once the buffer holds 8, and then none until the CPU has taken all 8
 *   (the fetch after that is non-sequential);
 * - an opcode fetch from the buffer takes 1 cycle; one of the halfword being
 *   read takes the rest of that read, except that a non-sequential fetch in
 *   the first cycle of the first read after a restart takes full
 *   non-sequential time;
 * - any other cartridge access abandons the read, and loses a cycle when a
 *   read completed in the cycle it starts in; a data access (or anything on
 *   the SRAM bus) empties the buffer, an opcode fetch restarts reading
 *   after itself;
 * - a read at a 128 KiB boundary never completes; the CPU fetches that
 *   halfword itself, non-sequentially.
 * The prefetcher is run lazily, at the next cartridge access, over the
 * cycles since the last one (pf_at). */

/* One cycle of the prefetcher; true if a read completed in it. */
static bool pf_tick(GbaMachine *m)
{
    if (!m->pf_check || m->pf_full) return false;
    if (m->pf_fetch == 0) {
        if (m->pf_count == 8) {
            m->pf_full = true;
            return false;
        }
        m->pf_wait = m->wait_s16[(m->pf_read >> 24) & 15u];
        m->pf_boundary = (m->pf_read & 0x1FFFEu) == 0;
        m->pf_fetch = 1;
        return false;
    }
    m->pf_following = true;
    if (++m->pf_fetch < m->pf_wait) return false;
    m->pf_fetch = 0;
    if (!m->pf_boundary) {
        m->pf_count++;
        m->pf_read += 2;
    }
    return true;
}

/* Run the prefetcher over the cycles from pf_at to now. */
static void pf_catch_up(GbaMachine *m)
{
    uint64_t n = m->cpu.cycles - m->pf_at;
    m->pf_at = m->cpu.cycles;
    while (n && m->pf_check && !m->pf_full) {
        if (m->pf_fetch == 0 || m->pf_boundary) {
            /* A read starting, or one spinning at a boundary, whose state
             * repeats every pf_wait cycles. */
            if (m->pf_boundary && n > 2u * m->pf_wait) n = m->pf_wait + n % m->pf_wait;
            pf_tick(m);
            n--;
            continue;
        }
        uint32_t left = (uint32_t)(m->pf_wait - m->pf_fetch);
        m->pf_following = true;
        if (n < left) {
            m->pf_fetch = (uint8_t)(m->pf_fetch + n);
            return;
        }
        n -= left;
        m->pf_fetch = 0;
        m->pf_count++;
        m->pf_read += 2;
    }
}

void gba_cart_access(GbaMachine *m, uint32_t addr, bool word, int access)
{
    pf_catch_up(m);
    const uint64_t start = m->cpu.cycles;
    const bool glitch = pf_tick(m); /* the prefetcher's step in this cycle comes first */
    const uint32_t region = (addr >> 24) & 15u;
    const bool seq = (access & GBA_ACCESS_SEQ) != 0;
    const uint32_t size = word ? 4u : 2u;
    uint32_t cost;
    bool bus_taken = true; /* the prefetcher sits the access out */

    if (region >= 14 || !(access & GBA_ACCESS_CODE)) {
        /* A data access (CPU or DMA), or anything on the SRAM bus. */
        cost = gba_wait_cycles(m, region, addr, word, seq) + glitch;
        m->pf_fetch = 0;
        m->pf_check = 0;
    } else if (m->pf_check && addr == m->pf_check) {
        const uint32_t s16 = m->wait_s16[region], n16 = m->wait_n16[region];
        if (m->pf_count) {
            /* From the buffer; both halves of an ARM opcode if it has them. */
            m->pf_check += 2;
            m->pf_count--;
            cost = 1;
            bus_taken = false;
            if (word) {
                if (m->pf_count) {
                    m->pf_check += 2;
                    m->pf_count--;
                } else {
                    /* The second half is being read: wait for it. */
                    cost = m->pf_fetch ? (uint32_t)(m->pf_wait - m->pf_fetch + 1) : 1u + s16;
                    m->pf_read += 2;
                    m->pf_check += 2;
                    m->pf_fetch = 0;
                    bus_taken = true;
                }
            }
            if (!bus_taken && m->pf_count == 0 && m->pf_full) {
                m->pf_check = 0;
                m->pf_force_nseq = true;
            }
        } else if ((addr & (word ? 0x1FFFCu : 0x1FFFEu)) == 0) {
            /* The prefetcher is stuck at a 128 KiB boundary. */
            cost = (word ? n16 + s16 : n16) + glitch;
            m->pf_read += size;
            m->pf_check += size;
            m->pf_fetch = 0;
        } else {
            /* The halfword being read (this cycle's step has started it if
             * nothing else had): the CPU takes over the rest of the read. */
            if (!seq && m->pf_fetch == 1 && !m->pf_following) cost = word ? n16 + s16 : n16;
            else cost = (uint32_t)(m->pf_wait - m->pf_fetch + 1) + (word ? s16 : 0u);
            m->pf_read += size;
            m->pf_check += size;
            m->pf_fetch = 0;
        }
    } else {
        /* Nothing to do with the prefetcher: a normal fetch, after which it
         * reads ahead from the next halfword. */
        cost = gba_wait_cycles(m, region, addr, word, seq && !m->pf_force_nseq) + glitch;
        m->pf_force_nseq = false;
        m->pf_count = 0;
        m->pf_fetch = 0;
        m->pf_full = false;
        m->pf_following = false;
        m->pf_check = m->pf_read = addr + size;
    }
    m->cpu.cycles = start + cost;
    m->pf_at = bus_taken ? m->cpu.cycles : start + 1;
}

/* ------------------------------------------------------------------ */
/* Open bus                                                            */
/* ------------------------------------------------------------------ */

static uint16_t code_half(GbaMachine *m, uint32_t addr)
{
    return (uint16_t)(gba_peek32(m, addr & ~3u) >> ((addr & 2u) * 8u));
}

/* Value a read from unmapped memory returns: the most recently prefetched
 * opcode(s), or the last DMA value while a DMA runs (GBATEK "Unpredictable
 * Things"). */
uint32_t gba_open_bus(GbaMachine *m)
{
    if (m->dma_active >= 0) return m->dma[m->dma_active].latch;

    const GbaCpu *c = &m->cpu;
    /* The latest fetches are in the pipeline when interpreting. Recompiled
     * code (ROM only, which never changes) does not maintain the pipeline
     * but keeps r[15] exact, so the same opcodes are read back from memory. */
    if (!(c->cpsr & GBA_CPSR_T)) return m->in_native ? gba_peek32(m, c->r[15] & ~3u) : c->pipe[1];

    /* Thumb: which halfwords the bus still holds depends on the region's
     * bus width and on the alignment of the latest fetch (pipe[1], from the
     * instruction address + 4). */
    uint32_t pc = c->r[15] & ~1u;
    uint32_t cur = m->in_native ? code_half(m, pc) : c->pipe[1] & 0xFFFFu;
    uint32_t prev = m->in_native ? code_half(m, pc - 2) : c->pipe[0] & 0xFFFFu;
    switch (pc >> 24) {
    case 0x0:
    case 0x7:
        if (pc & 2u) return (cur << 16) | prev;
        return ((uint32_t)code_half(m, pc + 2) << 16) | cur;
    case 0x3:
        if (pc & 2u) return (cur << 16) | prev;
        return (prev << 16) | cur;
    default:
        return (cur << 16) | cur;
    }
}

/* ------------------------------------------------------------------ */
/* Raw region access (no timing, no side effects outside I/O)          */
/* ------------------------------------------------------------------ */

static uint32_t rom_read(GbaMachine *m, uint32_t addr, int size)
{
    uint32_t off = addr & 0x1FFFFFFu;
    if (off + (uint32_t)size <= m->rom_size) {
        if (size == 4) return rd32(&m->rom[off]);
        if (size == 2) return rd16(&m->rom[off]);
        return m->rom[off];
    }
    /* Past the end of the ROM the cartridge bus returns the low address
     * lines: each halfword reads as (address / 2). */
    uint32_t lo = (addr >> 1) & 0xFFFFu;
    if (size == 4) return lo | (((lo + 1u) & 0xFFFFu) << 16);
    if (size == 2) return lo;
    return (lo >> ((addr & 1u) * 8u)) & 0xFFu;
}

/* Flash IDs: SST 39VF512 for 64 KiB, Macronix MX29L010 for 128 KiB. */
static uint8_t flash_id_byte(const GbaMachine *m, uint32_t off)
{
    if (m->save_type == GBA_SAVE_FLASH128) return off == 0 ? 0xC2u : 0x09u;
    return off == 0 ? 0xBFu : 0xD4u;
}

static uint32_t save_read8(GbaMachine *m, uint32_t addr)
{
    uint32_t off = addr & 0xFFFFu;
    switch (m->save_type) {
    case GBA_SAVE_SRAM:
        return m->save[addr & 0x7FFFu];
    case GBA_SAVE_FLASH64:
    case GBA_SAVE_FLASH128:
        if (m->flash_id_mode && off < 2) return flash_id_byte(m, off);
        return m->save[(uint32_t)m->flash_bank * 0x10000u + off];
    default:
        return 0xFFu;
    }
}

/* Flash command protocol (GBATEK "Backup Flash ROM"): every command is the
 * unlock sequence AA->5555, 55->2AAA followed by the command byte to 5555.
 * 0x80 arms an erase that a second unlocked 0x10 (chip) or 0x30 at a sector
 * (4 KiB) completes; 0xA0 programs the next byte written; 0xB0 selects the
 * bank with the next write to 0000 (128 KiB chips). */
static void flash_write(GbaMachine *m, uint32_t addr, uint8_t value)
{
    uint32_t off = addr & 0xFFFFu;
    uint32_t bank_base = (uint32_t)m->flash_bank * 0x10000u;

    if (m->flash_program_next) {
        m->save[bank_base + off] = value;
        m->save_dirty = true;
        m->flash_program_next = false;
        return;
    }
    if (m->flash_bank_next) {
        if (off == 0 && m->save_type == GBA_SAVE_FLASH128) m->flash_bank = value & 1u;
        m->flash_bank_next = false;
        return;
    }

    switch (m->flash_stage) {
    case 0:
        if (off == 0x5555u && value == 0xAAu) m->flash_stage = 1;
        return;
    case 1:
        m->flash_stage = (off == 0x2AAAu && value == 0x55u) ? 2 : 0;
        return;
    default:
        break;
    }
    m->flash_stage = 0;

    if (m->flash_erase_armed) {
        m->flash_erase_armed = false;
        if (off == 0x5555u && value == 0x10u) {
            memset(m->save, 0xFF, m->save_type == GBA_SAVE_FLASH128 ? 0x20000u : 0x10000u);
            m->save_dirty = true;
        } else if (value == 0x30u) {
            memset(&m->save[bank_base + (off & 0xF000u)], 0xFF, 0x1000);
            m->save_dirty = true;
        }
        return;
    }
    if (off != 0x5555u) return;
    switch (value) {
    case 0x90: m->flash_id_mode = true; break;
    case 0xF0: m->flash_id_mode = false; break;
    case 0x80: m->flash_erase_armed = true; break;
    case 0xA0: m->flash_program_next = true; break;
    case 0xB0: m->flash_bank_next = true; break;
    default: break;
    }
}

static void save_write8(GbaMachine *m, uint32_t addr, uint8_t value)
{
    switch (m->save_type) {
    case GBA_SAVE_SRAM:
        m->save[addr & 0x7FFFu] = value;
        m->save_dirty = true;
        break;
    case GBA_SAVE_FLASH64:
    case GBA_SAVE_FLASH128:
        flash_write(m, addr, value);
        break;
    default:
        break;
    }
}

static uint32_t io_read(GbaMachine *m, uint32_t addr, int size)
{
    /* Catch up on due events first, so status registers (DISPSTAT, VCOUNT,
     * IF, DMA enable bits) are exact even in the middle of a long block of
     * recompiled code. */
    if (m->cpu.cycles >= m->next_event) gba_process_events(m);
    uint32_t off = addr & 0x00FFFFFFu;
    if (off < GBA_IO_SIZE) {
        if (size == 4) return gba_io_read16(m, off) | ((uint32_t)gba_io_read16(m, off + 2) << 16);
        uint32_t half = gba_io_read16(m, off & ~1u);
        return size == 2 ? half : (half >> ((off & 1u) * 8u)) & 0xFFu;
    }
    if ((off & 0xFFFFu) >= 0x800u && (off & 0xFFFFu) < 0x804u) {
        uint32_t v = m->memcnt;
        if (size == 4) return v;
        uint32_t shift = (off & 3u) * 8u;
        return size == 2 ? (v >> shift) & 0xFFFFu : (v >> shift) & 0xFFu;
    }
    if (off == 0xFFF780u && m->debug_enabled) return size == 1 ? 0xEAu : 0x1DEAu;
    uint32_t bus = gba_open_bus(m);
    if (size == 4) return bus;
    return size == 2 ? (bus >> ((addr & 2u) * 8u)) & 0xFFFFu : (bus >> ((addr & 3u) * 8u)) & 0xFFu;
}

static uint32_t region_read(GbaMachine *m, uint32_t addr, int size)
{
    switch (addr >> 24) {
    case 0x0:
        if (addr < GBA_BIOS_SIZE) {
            if (gba_cpu_pc(&m->cpu) < GBA_BIOS_SIZE) {
                if (size == 4) return rd32(&m->bios[addr]);
                if (size == 2) return rd16(&m->bios[addr]);
                return m->bios[addr];
            }
            uint32_t v = m->bios_latch;
            if (size == 4) return v;
            return size == 2 ? (v >> ((addr & 2u) * 8u)) & 0xFFFFu : (v >> ((addr & 3u) * 8u)) & 0xFFu;
        }
        break;
    case 0x2: {
        uint32_t off = addr & (GBA_EWRAM_SIZE - 1);
        return size == 4 ? rd32(&m->ewram[off]) : size == 2 ? rd16(&m->ewram[off]) : m->ewram[off];
    }
    case 0x3: {
        uint32_t off = addr & (GBA_IWRAM_SIZE - 1);
        return size == 4 ? rd32(&m->iwram[off]) : size == 2 ? rd16(&m->iwram[off]) : m->iwram[off];
    }
    case 0x4:
        return io_read(m, addr, size);
    case 0x5: {
        uint32_t off = addr & (GBA_PALETTE_SIZE - 1);
        return size == 4 ? rd32(&m->palette[off]) : size == 2 ? rd16(&m->palette[off]) : m->palette[off];
    }
    case 0x6: {
        uint32_t off = vram_offset(addr);
        return size == 4 ? rd32(&m->vram[off]) : size == 2 ? rd16(&m->vram[off]) : m->vram[off];
    }
    case 0x7: {
        uint32_t off = addr & (GBA_OAM_SIZE - 1);
        return size == 4 ? rd32(&m->oam[off]) : size == 2 ? rd16(&m->oam[off]) : m->oam[off];
    }
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
        if (gba_eeprom_address(m, addr)) return gba_eeprom_read_bit(m);
        if (gba_gpio_address(m, addr)) return gba_gpio_read(m, addr, size, rom_read(m, addr, size));
        return rom_read(m, addr, size);
    case 0xE: case 0xF: {
        /* 8-bit bus: wider reads see the byte repeated. */
        uint32_t b = save_read8(m, addr);
        return size == 4 ? b * 0x01010101u : size == 2 ? b * 0x0101u : b;
    }
    default:
        break;
    }
    uint32_t bus = gba_open_bus(m);
    if (size == 4) return bus;
    return size == 2 ? (bus >> ((addr & 2u) * 8u)) & 0xFFFFu : (bus >> ((addr & 3u) * 8u)) & 0xFFu;
}

static void io_write(GbaMachine *m, uint32_t addr, uint32_t value, int size)
{
    uint32_t off = addr & 0x00FFFFFFu;
    if (off < GBA_IO_SIZE) {
        if (size == 4) {
            gba_io_write16(m, off, (uint16_t)value, 0xFFFFu);
            gba_io_write16(m, off + 2, (uint16_t)(value >> 16), 0xFFFFu);
        } else if (size == 2) {
            gba_io_write16(m, off, (uint16_t)value, 0xFFFFu);
        } else {
            uint32_t shift = (off & 1u) * 8u;
            gba_io_write16(m, off & ~1u, (uint16_t)((value & 0xFFu) << shift), (uint16_t)(0xFFu << shift));
        }
        return;
    }
    if ((off & 0xFFFFu) >= 0x800u && (off & 0xFFFFu) < 0x804u) {
        uint32_t shift = (off & 3u) * 8u;
        uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu << shift : 0xFFu << shift;
        m->memcnt = (m->memcnt & ~mask) | ((value << (size == 4 ? 0 : shift)) & mask);
        gba_update_waitstates(m);
        return;
    }
    /* mGBA debug output port. */
    if (off == 0xFFF780u) {
        m->debug_enabled = (value & 0xFFFFu) == 0xC0DEu;
        return;
    }
    if (m->debug_enabled && off >= 0xFFF600u && off < 0xFFF700u) {
        uint32_t i = off - 0xFFF600u;
        for (int b = 0; b < size && i + (uint32_t)b < 0x100u; b++) {
            m->debug_string[i + (uint32_t)b] = (char)(value >> (b * 8));
        }
        return;
    }
    if (m->debug_enabled && off == 0xFFF700u && (value & 0x100u)) {
        m->debug_string[0x100] = '\0';
        if (m->debug_log) m->debug_log(m, (int)(value & 7u), m->debug_string);
        memset(m->debug_string, 0, sizeof m->debug_string);
    }
}

static void region_write(GbaMachine *m, uint32_t addr, uint32_t value, int size)
{
    switch (addr >> 24) {
    case 0x2: {
        uint32_t off = addr & (GBA_EWRAM_SIZE - 1);
        if (m->rc_code_pages[off >> 8]) m->rc_ram_gen++;
        if (size == 4) wr32(&m->ewram[off], value);
        else if (size == 2) wr16(&m->ewram[off], (uint16_t)value);
        else m->ewram[off] = (uint8_t)value;
        return;
    }
    case 0x3: {
        uint32_t off = addr & (GBA_IWRAM_SIZE - 1);
        if (m->rc_code_pages[1024 + (off >> 8)]) m->rc_ram_gen++;
        if (size == 4) wr32(&m->iwram[off], value);
        else if (size == 2) wr16(&m->iwram[off], (uint16_t)value);
        else m->iwram[off] = (uint8_t)value;
        return;
    }
    case 0x4:
        io_write(m, addr, value, size);
        return;
    case 0x5: {
        uint32_t off = addr & (GBA_PALETTE_SIZE - 1);
        if (size == 4) wr32(&m->palette[off], value);
        else if (size == 2) wr16(&m->palette[off], (uint16_t)value);
        else wr16(&m->palette[off & ~1u], (uint16_t)((value & 0xFFu) * 0x0101u)); /* byte fills the halfword */
        return;
    }
    case 0x6: {
        uint32_t off = vram_offset(addr);
        if (size == 4) {
            wr32(&m->vram[off], value);
        } else if (size == 2) {
            wr16(&m->vram[off], (uint16_t)value);
        } else {
            /* Byte writes fill the halfword in BG memory and are ignored in
             * OBJ memory, whose start depends on the video mode. */
            uint32_t mode = m->io[0] & 7u;
            uint32_t obj_start = mode >= 3 ? 0x14000u : 0x10000u;
            if (off < obj_start) wr16(&m->vram[off & ~1u], (uint16_t)((value & 0xFFu) * 0x0101u));
        }
        return;
    }
    case 0x7: {
        uint32_t off = addr & (GBA_OAM_SIZE - 1);
        if (size == 4) wr32(&m->oam[off], value);
        else if (size == 2) wr16(&m->oam[off], (uint16_t)value);
        /* byte writes to OAM are ignored */
        return;
    }
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
        /* ROM is read-only; the EEPROM and the GPIO port listen here. */
        if (gba_eeprom_address(m, addr)) gba_eeprom_write_bit(m, value);
        else if (gba_gpio_address(m, addr)) gba_gpio_write(m, addr, value, size);
        return;
    case 0xE: case 0xF: {
        /* 8-bit bus: wider writes store the byte selected by the address. */
        uint32_t shift = (addr & (uint32_t)(size - 1)) * 8u;
        save_write8(m, addr, (uint8_t)(value >> shift));
        return;
    }
    default:
        return; /* BIOS and unmapped writes are ignored */
    }
}

/* ------------------------------------------------------------------ */
/* CPU-facing bus                                                      */
/* ------------------------------------------------------------------ */

static inline void note_code_fetch(GbaMachine *m, uint32_t addr, int access)
{
    if ((access & GBA_ACCESS_CODE) && addr < GBA_BIOS_SIZE) {
        /* Once the PC leaves the BIOS, reads of it keep returning the last
         * opcode it fetched. */
        m->bios_latch = rd32(&m->bios[addr & (GBA_BIOS_SIZE - 4)]);
    }
}

/* Every region except the 8-bit save bus ignores the low address bits. */
static inline uint32_t bus_align(uint32_t addr, uint32_t size)
{
    return (addr >> 25) == 7u ? addr : addr & ~(size - 1u);
}

/* Data accesses (anything but an opcode fetch, DMA included) leave the next
 * opcode fetch non-sequential. */
uint32_t gba_bus_read8(GbaCpu *cpu, uint32_t addr, int access)
{
    GbaMachine *m = GBA_MACHINE(cpu);
    gba_access_cycles(m, addr, false, access);
    m->fetch_nseq = true;
    return region_read(m, addr, 1);
}

uint32_t gba_bus_read16(GbaCpu *cpu, uint32_t addr, int access)
{
    GbaMachine *m = GBA_MACHINE(cpu);
    addr = bus_align(addr, 2);
    gba_access_cycles(m, addr, false, access);
    if (!(access & GBA_ACCESS_CODE)) m->fetch_nseq = true;
    note_code_fetch(m, addr, access);
    return region_read(m, addr, 2);
}

uint32_t gba_bus_read32(GbaCpu *cpu, uint32_t addr, int access)
{
    GbaMachine *m = GBA_MACHINE(cpu);
    addr = bus_align(addr, 4);
    gba_access_cycles(m, addr, true, access);
    if (!(access & GBA_ACCESS_CODE)) m->fetch_nseq = true;
    note_code_fetch(m, addr, access);
    return region_read(m, addr, 4);
}

void gba_bus_write8(GbaCpu *cpu, uint32_t addr, uint8_t value, int access)
{
    GbaMachine *m = GBA_MACHINE(cpu);
    gba_access_cycles(m, addr, false, access);
    m->fetch_nseq = true;
    region_write(m, addr, value, 1);
}

void gba_bus_write16(GbaCpu *cpu, uint32_t addr, uint16_t value, int access)
{
    GbaMachine *m = GBA_MACHINE(cpu);
    addr = bus_align(addr, 2);
    gba_access_cycles(m, addr, false, access);
    m->fetch_nseq = true;
    region_write(m, addr, value, 2);
}

void gba_bus_write32(GbaCpu *cpu, uint32_t addr, uint32_t value, int access)
{
    GbaMachine *m = GBA_MACHINE(cpu);
    addr = bus_align(addr, 4);
    gba_access_cycles(m, addr, true, access);
    m->fetch_nseq = true;
    region_write(m, addr, value, 4);
}

/* After internal cycles, too, the next opcode fetch is non-sequential. */
void gba_bus_idle(GbaCpu *cpu, int cycles)
{
    cpu->cycles += (uint64_t)cycles;
    GBA_MACHINE(cpu)->fetch_nseq = true;
}

/* ------------------------------------------------------------------ */
/* Debugger access                                                     */
/* ------------------------------------------------------------------ */

uint32_t gba_peek32(GbaMachine *m, uint32_t addr)
{
    addr &= ~3u;
    switch (addr >> 24) {
    case 0x0: return addr < GBA_BIOS_SIZE ? rd32(&m->bios[addr]) : 0;
    case 0x2: return rd32(&m->ewram[addr & (GBA_EWRAM_SIZE - 1)]);
    case 0x3: return rd32(&m->iwram[addr & (GBA_IWRAM_SIZE - 1)]);
    case 0x4: return (addr & 0xFFFFFFu) < GBA_IO_SIZE ? rd32(&m->io[addr & 0x3FCu]) : 0;
    case 0x5: return rd32(&m->palette[addr & (GBA_PALETTE_SIZE - 1)]);
    case 0x6: return rd32(&m->vram[vram_offset(addr)]);
    case 0x7: return rd32(&m->oam[addr & (GBA_OAM_SIZE - 1)]);
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
        return rom_read(m, addr, 4);
    default: return 0;
    }
}

uint8_t gba_peek8(GbaMachine *m, uint32_t addr)
{
    return (uint8_t)(gba_peek32(m, addr) >> ((addr & 3u) * 8u));
}
