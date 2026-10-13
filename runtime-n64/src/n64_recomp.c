/*
 * Recompiled-program installation, entry lookup, code checks and
 * native-code yields (see n64_recomp.h).
 *
 * Lookup goes through an open-addressing hash of the entry addresses built
 * at install time; entries with the same address (code compiled from
 * different overlays loaded there) sit in the same probe sequence, and the
 * first whose bytes are in RDRAM wins.
 *
 * RDRAM is split into 64-byte granules. Install marks the granules each
 * function's bytes cover; a store or DMA into a marked granule clears the
 * "verified" flag of every function there and bumps rc_invalidations, so
 * native code that just changed its own bytes returns to the run loop.
 */
#include "n64_recomp.h"

#include <stdlib.h>
#include <string.h>

#include "n64_sha256.h"

#define GRANULE N64_CODE_GRANULE_SHIFT

static uint32_t hash_key(uint32_t key)
{
    key ^= key >> 15;
    key *= 0x2C1B3C6Du;
    key ^= key >> 12;
    key *= 0x297A2D39u;
    key ^= key >> 15;
    return key;
}

static void release(N64Machine *m)
{
    free(m->rc_hash);
    free(m->rc_code_map);
    free(m->rc_granule_fns);
    free(m->rc_granule_list);
    free(m->rc_verified);
    m->rc_hash = NULL;
    m->rc_code_map = NULL;
    m->rc_granule_fns = NULL;
    m->rc_granule_list = NULL;
    m->rc_verified = NULL;
    m->rc_hash_mask = 0;
    m->rc_program = NULL;
    m->cpu.links.code_map = NULL;
}

bool n64_rc_install(N64Machine *m, const N64RcProgram *p)
{
    release(m);
    if (!p) return true;
    if (!m->rom || m->rom_size != p->rom_size) return false;
    char digest[65];
    n64_sha256_hex(m->rom, m->rom_size, digest);
    if (strcmp(digest, p->rom_sha256) != 0) return false;

    uint32_t size = 16;
    while (size < p->entry_count * 2u) size <<= 1;
    m->rc_hash = malloc(size * sizeof *m->rc_hash);
    const uint32_t granules = m->rdram_size >> GRANULE;
    m->rc_code_map = calloc(granules, 1);
    m->rc_granule_fns = calloc(granules + 1u, sizeof *m->rc_granule_fns);
    m->rc_verified = calloc(p->block_count ? p->block_count : 1u, 1);
    if (!m->rc_hash || !m->rc_code_map || !m->rc_granule_fns || !m->rc_verified) {
        release(m);
        return false;
    }
    for (uint32_t i = 0; i < size; i++) m->rc_hash[i] = UINT32_MAX;
    m->rc_hash_mask = size - 1u;
    for (uint32_t i = 0; i < p->entry_count; i++) {
        uint32_t slot = hash_key(p->entries[i].vaddr) & m->rc_hash_mask;
        while (m->rc_hash[slot] != UINT32_MAX) slot = (slot + 1u) & m->rc_hash_mask;
        m->rc_hash[slot] = i;
    }

    /* Granule -> blocks, as offsets into one list (counted, then filled). */
    uint32_t total = 0;
    for (uint32_t b = 0; b < p->block_count; b++) {
        const N64RcBlock *blk = &p->blocks[b];
        if (!blk->size || blk->paddr + blk->size > m->rdram_size) continue;
        for (uint32_t g = blk->paddr >> GRANULE; g <= (blk->paddr + blk->size - 1u) >> GRANULE; g++) {
            m->rc_granule_fns[g + 1u]++;
            total++;
        }
    }
    for (uint32_t g = 0; g < granules; g++) m->rc_granule_fns[g + 1u] += m->rc_granule_fns[g];
    m->rc_granule_list = malloc((total ? total : 1u) * sizeof *m->rc_granule_list);
    uint32_t *fill = calloc(granules, sizeof *fill);
    if (!m->rc_granule_list || !fill) {
        free(fill);
        release(m);
        return false;
    }
    for (uint32_t b = 0; b < p->block_count; b++) {
        const N64RcBlock *blk = &p->blocks[b];
        if (!blk->size || blk->paddr + blk->size > m->rdram_size) continue;
        for (uint32_t g = blk->paddr >> GRANULE; g <= (blk->paddr + blk->size - 1u) >> GRANULE; g++) {
            m->rc_granule_list[m->rc_granule_fns[g] + fill[g]++] = b;
            m->rc_code_map[g] = 1;
        }
    }
    free(fill);
    m->rc_program = p;
    m->cpu.links.code_map = m->rc_code_map;
    return true;
}

N64RcFunc n64_rc_lookup(N64Machine *m, uint64_t pc)
{
    const N64RcProgram *p = m->rc_program;
    if ((uint64_t)(int64_t)(int32_t)(uint32_t)pc != pc) return NULL;
    const uint32_t key = (uint32_t)pc;
    uint32_t paddr = 0;
    bool translated = false;
    uint32_t slot = hash_key(key) & m->rc_hash_mask;
    for (;;) {
        uint32_t index = m->rc_hash[slot];
        if (index == UINT32_MAX) return NULL;
        const N64RcEntry *e = &p->entries[index];
        if (e->vaddr == key) {
            if (!translated) {
                if (!n64_cpu_translate_quiet(&m->cpu, pc, &paddr)) return NULL;
                translated = true;
            }
            const N64RcBlock *blk = &p->blocks[e->block];
            if (paddr >= blk->paddr && paddr < blk->paddr + blk->size) {
                if (!m->rc_verified[e->block]) {
                    if (blk->paddr + blk->size <= m->rdram_size &&
                        memcmp(m->rdram + blk->paddr, blk->bytes, blk->size) == 0)
                        m->rc_verified[e->block] = 1;
                }
                if (m->rc_verified[e->block]) return e->fn;
            }
        }
        slot = (slot + 1u) & m->rc_hash_mask;
    }
}

void n64_bus_code_written(N64Cpu *c, uint32_t paddr, uint32_t size)
{
    N64Machine *m = N64_MACHINE(c);
    if (!m->rc_code_map || !size) return;
    const uint32_t granules = m->rdram_size >> GRANULE;
    uint32_t last = (paddr + size - 1u) >> GRANULE;
    for (uint32_t g = paddr >> GRANULE; g <= last && g < granules; g++) {
        if (!m->rc_code_map[g]) continue;
        for (uint32_t k = m->rc_granule_fns[g]; k < m->rc_granule_fns[g + 1u]; k++) m->rc_verified[m->rc_granule_list[k]] = 0;
        m->rc_invalidations++;
    }
}

bool n64_rc_sync(N64Cpu *c)
{
    N64Machine *m = N64_MACHINE(c);
    if (c->cycles >= m->next_event) n64_process_events(m);
    return c->next_sync == 0 || m->frame_ready;
}
