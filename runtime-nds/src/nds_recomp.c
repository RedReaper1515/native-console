/*
 * Recompiled-program installation, entry lookup and the call into native
 * code (see nds_recomp.h).
 */
#include "nds_recomp.h"

#include <stdlib.h>
#include <string.h>

/* What is known about one block: whether the code it was compiled from was
 * in memory when last compared, and enough to tell cheaply whether that can
 * have changed. */
struct NdsRcBlockState {
    uint64_t stamp;   /* the latest sequence number among its pages and the map, when last compared */
    uint64_t map_seq; /* the memory map `first`, `last` and `exact` were worked out for; 0 = not yet */
    uint16_t first;   /* the pages of the code space it lies in under that map; */
    uint16_t last;    /* first > last = none (a BIOS ROM, or not mapped) */
    uint8_t valid;    /* its bytes were in memory at `stamp` */
    uint8_t exact;    /* its ranges are far apart: look at each range's pages, not all between */
};

/* Fibonacci hashing of an entry key into a CPU's lookup table. */
static inline uint32_t entry_hash(const NdsMachine *m, int cpu, uint32_t key)
{
    return (key * 0x9E3779B1u) >> m->rc_index_shift[cpu];
}

static inline bool word_marked(const NdsMachine *m, uint32_t offset)
{
    return ((m->rc_code_words[offset >> 5] >> ((offset >> 2) & 7u)) & 1u) != 0;
}

static inline void mark_word(NdsMachine *m, uint32_t offset)
{
    m->rc_code_words[offset >> 5] |= (uint8_t)(1u << ((offset >> 2) & 7u));
}

/* Where the bytes of [addr, addr + size) are for an opcode fetch by `cpu`
 * with the memory map as it is now: a host pointer, or NULL if the range is
 * not all in one piece of memory that code can be compiled for. *offset gets
 * the range's place in the code space, or NDS_RC_SPACE for the BIOS ROMs,
 * which cannot be written. */
static const uint8_t *code_ptr(const NdsMachine *m, int cpu, uint32_t addr, uint32_t size, uint32_t *offset)
{
    if (size == 0 || size > NDS_MAIN_RAM_SIZE) return NULL;
    if (cpu == NDS_ARM9) {
        if (addr < m->itcm_end && !m->itcm_load) {
            uint32_t at = addr & (NDS_ITCM_SIZE - 1u);
            if (m->itcm_end - addr < size || at + size > NDS_ITCM_SIZE) return NULL;
            *offset = NDS_RC_ITCM + at;
            return m->itcm + at;
        }
        switch (addr >> 24) {
        case 0x02: {
            uint32_t at = addr & (NDS_MAIN_RAM_SIZE - 1u);
            if (at + size > NDS_MAIN_RAM_SIZE) return NULL;
            *offset = NDS_RC_MAIN + at;
            return m->main_ram + at;
        }
        case 0x03: {
            if (!m->wram9) return NULL;
            uint32_t at = addr & m->wram9_mask;
            if (at + size > m->wram9_mask + 1u) return NULL;
            *offset = NDS_RC_SHARED + (uint32_t)(m->wram9 - m->shared_wram) + at;
            return m->wram9 + at;
        }
        case 0xFF: {
            uint32_t at = addr & 0xFFFFu;
            if ((addr & 0xFFFF0000u) != 0xFFFF0000u || at + size > NDS_BIOS9_SIZE) return NULL;
            *offset = NDS_RC_SPACE;
            return m->bios9 + at;
        }
        default:
            return NULL;
        }
    }

    switch (addr >> 24) {
    case 0x00:
        if (addr >= NDS_BIOS7_SIZE || NDS_BIOS7_SIZE - addr < size) return NULL;
        *offset = NDS_RC_SPACE;
        return m->bios7 + addr;
    case 0x02: {
        uint32_t at = addr & (NDS_MAIN_RAM_SIZE - 1u);
        if (at + size > NDS_MAIN_RAM_SIZE) return NULL;
        *offset = NDS_RC_MAIN + at;
        return m->main_ram + at;
    }
    case 0x03:
        if (!(addr & 0x00800000u) && m->wram7) {
            uint32_t at = addr & m->wram7_mask;
            if (at + size > m->wram7_mask + 1u) return NULL;
            *offset = NDS_RC_SHARED + (uint32_t)(m->wram7 - m->shared_wram) + at;
            return m->wram7 + at;
        } else {
            uint32_t at = addr & (NDS_ARM7_WRAM_SIZE - 1u);
            if (at + size > NDS_ARM7_WRAM_SIZE) return NULL;
            *offset = NDS_RC_ARM7_WRAM + at;
            return m->arm7_wram + at;
        }
    default:
        return NULL;
    }
}

static inline uint64_t latest_in(const NdsMachine *m, uint32_t first, uint32_t last, uint64_t stamp)
{
    for (uint32_t page = first; page <= last; page++) {
        if (m->rc_page_seq[page] > stamp) stamp = m->rc_page_seq[page];
    }
    return stamp;
}

static bool block_check(NdsMachine *m, int cpu, uint32_t index);

/* Is the code a block was compiled from in memory now? The answer is kept
 * with the sequence number it holds as of: the latest write to code in any
 * page the block lies in, or the latest change of the memory map. While
 * that does not move, neither does the answer.
 *
 * This runs for every call into native code, so the usual case is a few
 * loads: the pages a block lies in under the present map are remembered
 * (nearly always one or two), and their sequence numbers are compared with
 * the one the answer was reached at. */
static inline bool block_valid(NdsMachine *m, int cpu, uint32_t index)
{
    const struct NdsRcBlockState *b = &m->rc_block[index];
    if (b->map_seq == m->rc_map_seq && !b->exact && b->stamp == latest_in(m, b->first, b->last, b->map_seq)) {
        return b->valid != 0;
    }
    return block_check(m, cpu, index);
}

/* The long way: find the block's ranges under the present map, and if a page
 * of it has been written since the last comparison, compare again. */
static bool block_check(NdsMachine *m, int cpu, uint32_t index)
{
    const NdsRcProgram *p = m->rc_program;
    const NdsRcBlock *block = &p->blocks[index];
    const NdsRcRange *range = &p->ranges[block->first_range];
    struct NdsRcBlockState *b = &m->rc_block[index];

    uint64_t stamp = m->rc_map_seq;
    uint32_t first = 1, last = 0, pages = 0;
    bool mapped = true;
    for (uint32_t i = 0; i < block->range_count && mapped; i++) {
        uint32_t offset;
        if (!code_ptr(m, cpu, range[i].addr, range[i].size, &offset)) {
            mapped = false;
        } else if (offset != NDS_RC_SPACE) {
            uint32_t lo = offset >> 12, hi = (offset + range[i].size - 1u) >> 12;
            stamp = latest_in(m, lo, hi, stamp);
            pages += hi - lo + 1u;
            if (first > last) {
                first = lo;
                last = hi;
            } else {
                if (lo < first) first = lo;
                if (hi > last) last = hi;
            }
        }
    }
    /* Not all there: only a change of the map can bring it back. */
    if (!mapped) {
        stamp = m->rc_map_seq;
        first = 1;
        last = 0;
    }
    /* When the ranges lie close together, everything from the first page to
     * the last is watched instead, which block_valid() can do by itself. A
     * write between the ranges then costs a comparison it did not need. */
    const bool exact = first <= last && last - first + 1u > pages + 2u;
    if (!exact) stamp = latest_in(m, first, last, m->rc_map_seq);
    if (b->map_seq == m->rc_map_seq && b->stamp == stamp) return b->valid != 0;

    bool valid = mapped;
    uint64_t hash = NDS_RC_HASH_INIT;
    for (uint32_t i = 0; i < block->range_count && valid; i++) {
        uint32_t offset;
        const uint8_t *bytes = code_ptr(m, cpu, range[i].addr, range[i].size, &offset);
        /* Writes are only noticed in words marked at install. If the map
         * has put this code somewhere that was not foreseen then, it cannot
         * be watched, so it is not run natively. */
        if (offset != NDS_RC_SPACE) {
            for (uint32_t at = offset & ~3u; at < offset + range[i].size && valid; at += 4) {
                valid = word_marked(m, at);
            }
        }
        hash = nds_rc_hash(hash, bytes, range[i].size);
    }
    if (mapped) m->rc_compared++;
    valid = valid && hash == block->hash;
    b->stamp = stamp;
    b->map_seq = m->rc_map_seq;
    b->first = (uint16_t)first;
    b->last = (uint16_t)last;
    b->exact = exact;
    b->valid = valid;
    return valid;
}

static void forget_lookups(NdsMachine *m)
{
    memset(m->rc_cache, 0xFF, sizeof m->rc_cache);
    m->rc_resume[NDS_ARM9] = m->rc_resume[NDS_ARM7] = NULL;
}

static void remove_program(NdsMachine *m)
{
    m->rc_program = NULL;
    for (int cpu = 0; cpu < 2; cpu++) {
        free(m->rc_index[cpu]);
        m->rc_index[cpu] = NULL;
    }
    free(m->rc_block);
    free(m->rc_code_words);
    m->rc_block = NULL;
    m->rc_code_words = NULL;
    forget_lookups(m);
}

/* Mark the words of the code space a range of guest addresses can lie in.
 * The memory map is not known yet (and changes), so a range is marked in
 * every piece of RAM its address can mean: that is one place, except in the
 * 0x03000000 area, where WRAMCNT decides. */
static void mark_code(NdsMachine *m, int cpu, uint32_t addr, uint32_t size)
{
    for (uint32_t a = addr & ~3u; a < addr + size; a += 4) {
        if (cpu == NDS_ARM9 && a < 0x02000000u) {
            mark_word(m, NDS_RC_ITCM + (a & (NDS_ITCM_SIZE - 1u)));
        } else if ((a >> 24) == 0x02u) {
            mark_word(m, NDS_RC_MAIN + (a & (NDS_MAIN_RAM_SIZE - 1u)));
        } else if ((a >> 24) == 0x03u) {
            /* Either half of the shared WRAM, or (ARM7) its own WRAM, which
             * also shows through below 0x03800000 when it has none of the
             * shared WRAM. */
            mark_word(m, NDS_RC_SHARED + (a & 0x3FFFu));
            mark_word(m, NDS_RC_SHARED + 0x4000u + (a & 0x3FFFu));
            if (cpu == NDS_ARM7) mark_word(m, NDS_RC_ARM7_WRAM + (a & (NDS_ARM7_WRAM_SIZE - 1u)));
        }
        /* Else the BIOS ROMs, or nowhere code can be compiled for. */
    }
}

bool nds_rc_install(NdsMachine *m, const NdsRcProgram *program)
{
    remove_program(m);
    if (!program) return true;

    m->rc_block = calloc(program->block_count ? program->block_count : 1u, sizeof *m->rc_block);
    m->rc_code_words = calloc(NDS_RC_SPACE >> 5, 1);
    bool ok = m->rc_block && m->rc_code_words;

    for (int cpu = 0; cpu < 2 && ok; cpu++) {
        /* At most half full, so a probe finds its key or an empty slot soon. */
        uint32_t bits = 1;
        while ((1u << bits) < program->entry_count[cpu] * 2u) bits++;
        m->rc_index[cpu] = calloc((size_t)1 << bits, sizeof *m->rc_index[cpu]);
        if (!m->rc_index[cpu]) {
            ok = false;
            break;
        }
        m->rc_index_shift[cpu] = 32u - bits;
        m->rc_index_mask[cpu] = (1u << bits) - 1u;
        const NdsRcEntry *entries = program->entries[cpu];
        for (uint32_t i = 0; i < program->entry_count[cpu]; i++) {
            const NdsRcBlock *block = &program->blocks[entries[i].block];
            for (uint32_t r = 0; r < block->range_count; r++) {
                const NdsRcRange *range = &program->ranges[block->first_range + r];
                mark_code(m, cpu, range->addr, range->size);
            }
            if (i > 0 && entries[i].key == entries[i - 1].key) continue; /* found through the first */
            uint32_t h = entry_hash(m, cpu, entries[i].key);
            while (m->rc_index[cpu][h]) h = (h + 1u) & m->rc_index_mask[cpu];
            m->rc_index[cpu][h] = i + 1u;
        }
    }
    if (!ok) {
        remove_program(m);
        return false;
    }
    /* A block's map_seq starts at zero, which is before any sequence
     * number: every block is unchecked. */
    m->rc_map_seq = ++m->rc_seq;
    m->rc_program = program;
    return true;
}

void nds_rc_invalidate(NdsMachine *m)
{
    m->rc_map_seq = ++m->rc_seq;
    forget_lookups(m);
}

NdsRcFunc nds_rc_lookup(NdsMachine *m, int cpu, uint32_t key, uint32_t *block)
{
    /* The last function found for an address is nearly always the one
     * wanted again; it still has to be in memory. */
    struct NdsRcCacheEntry *cached = &m->rc_cache[cpu][(key >> 1) & (NDS_RC_CACHE - 1u)];
    if (cached->key == key && block_valid(m, cpu, cached->block)) {
        *block = cached->block;
        return cached->fn;
    }

    const NdsRcProgram *p = m->rc_program;
    const NdsRcEntry *entries = p->entries[cpu];
    const uint32_t count = p->entry_count[cpu];
    for (uint32_t h = entry_hash(m, cpu, key);; h = (h + 1u) & m->rc_index_mask[cpu]) {
        uint32_t index = m->rc_index[cpu][h];
        if (!index) return NULL;
        if (entries[index - 1u].key != key) continue;
        /* One candidate per piece of code that can be at this address. */
        for (uint32_t i = index - 1u; i < count && entries[i].key == key; i++) {
            if (block_valid(m, cpu, entries[i].block)) {
                cached->key = key;
                cached->block = entries[i].block;
                cached->fn = entries[i].fn;
                *block = entries[i].block;
                return entries[i].fn;
            }
        }
        return NULL;
    }
}

bool nds_rc_run(NdsMachine *m, int cpu)
{
    NdsCpu *c = cpu == NDS_ARM9 ? &m->arm9 : &m->arm7;
    const uint32_t pc = nds_cpu_pc(c);
    const uint8_t thumb = (c->cpsr & NDS_CPSR_T) ? 1u : 0u;
    const uint64_t before = m->instructions[cpu];
    NdsRcFunc fn = m->rc_resume[cpu];
    uint32_t block = m->rc_resume_block[cpu];

    /* A function that yielded part-way carries on from any of its
     * instructions, as long as its code is still what is in memory. Given
     * an address that is not one of them, it returns having run nothing. */
    if (fn && m->rc_resume_thumb[cpu] == thumb && block_valid(m, cpu, block)) {
        c->branched = false;
        fn(m, pc);
    }
    if (m->instructions[cpu] == before) {
        fn = nds_rc_lookup(m, cpu, pc | thumb, &block);
        if (fn) {
            c->branched = false;
            fn(m, pc);
        }
        if (m->instructions[cpu] == before) {
            m->rc_resume[cpu] = NULL;
            return false;
        }
    }
    m->rc_calls[cpu]++;

    m->pipe_stale[cpu] = true;
    if (c->branched) {
        /* It charged every fetch up to the branch. The two that refill the
         * pipeline at the target are charged here as the interpreter's
         * refill charges them; the opcodes themselves are only read if the
         * interpreter turns out to be what runs next. */
        const uint32_t target = nds_cpu_pc(c);
        const uint32_t size = (c->cpsr & NDS_CPSR_T) ? 2u : 4u;
        if (cpu == NDS_ARM9) c->cycles += nds9_code_cycles(m, target) + nds9_code_cycles(m, target + size);
        else c->cycles += 2u * NDS7_CYCLES_ACCESS;
        c->branched = false;
        m->rc_resume[cpu] = NULL;
    } else {
        m->rc_resume[cpu] = fn;
        m->rc_resume_block[cpu] = block;
        m->rc_resume_thumb[cpu] = thumb;
    }
    return true;
}
