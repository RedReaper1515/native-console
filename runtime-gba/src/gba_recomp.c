/*
 * Recompiled-program installation and entry lookup (see gba_recomp.h).
 *
 * RAM code is verified lazily: every write to a RAM page that holds
 * compiled code bumps m->rc_ram_gen (gba_bus.c); a block is compared with
 * live RAM again only when the generation moved since it was last found
 * intact.
 *
 * The run loop looks an entry up for every native call, so lookups go
 * through an open-addressing hash of the keys built at install.
 */
#include "gba_recomp.h"

#include <stdlib.h>
#include <string.h>

#include "gba_sha256.h"

/* Page index for the code-page map: EWRAM pages 0-1023, IWRAM 1024-1151. */
static int ram_page(uint32_t addr)
{
    if ((addr >> 24) == 0x2u) return (int)((addr & (GBA_EWRAM_SIZE - 1)) >> 8);
    if ((addr >> 24) == 0x3u) return 1024 + (int)((addr & (GBA_IWRAM_SIZE - 1)) >> 8);
    return -1;
}

static const uint8_t *ram_ptr(const GbaMachine *m, uint32_t addr)
{
    if ((addr >> 24) == 0x2u) return &m->ewram[addr & (GBA_EWRAM_SIZE - 1)];
    return &m->iwram[addr & (GBA_IWRAM_SIZE - 1)];
}

/* Fibonacci hashing of an entry key into the lookup table. */
static inline uint32_t entry_hash(const GbaMachine *m, uint32_t key)
{
    return (key * 0x9E3779B1u) >> m->rc_index_shift;
}

bool gba_rc_install(GbaMachine *m, const GbaRcProgram *program)
{
    m->rc_program = NULL;
    free(m->rc_verified);
    m->rc_verified = NULL;
    free(m->rc_index);
    m->rc_index = NULL;
    memset(m->rc_code_pages, 0, sizeof m->rc_code_pages);
    if (!program) return true;
    if (!m->rom || m->rom_size != program->rom_size) return false;
    char digest[65];
    gba_sha256_hex(m->rom, m->rom_size, digest);
    if (strcmp(digest, program->rom_sha256) != 0) return false;

    if (program->ram_block_count) {
        m->rc_verified = calloc(program->ram_block_count, sizeof *m->rc_verified);
        if (!m->rc_verified) return false;
        for (uint32_t b = 0; b < program->ram_block_count; b++) {
            const GbaRcRamBlock *blk = &program->ram_blocks[b];
            for (uint32_t a = blk->addr & ~0xFFu; a < blk->addr + blk->size; a += 0x100u) {
                int page = ram_page(a);
                if (page >= 0) m->rc_code_pages[page] = 1;
            }
        }
    }
    /* At most half full, so a probe finds its key or an empty slot soon. */
    uint32_t bits = 1;
    while ((1u << bits) < program->entry_count * 2u) bits++;
    m->rc_index = calloc((size_t)1 << bits, sizeof *m->rc_index);
    if (!m->rc_index) return false;
    m->rc_index_shift = 32u - bits;
    m->rc_index_mask = (1u << bits) - 1u;
    for (uint32_t i = 0; i < program->entry_count; i++) {
        uint32_t h = entry_hash(m, program->entries[i].key);
        while (m->rc_index[h]) h = (h + 1u) & m->rc_index_mask;
        m->rc_index[h] = i + 1u;
    }

    m->rc_ram_gen = 1; /* every block starts unverified (generation 0) */
    m->rc_program = program;
    return true;
}

GbaRcFunc gba_rc_lookup(GbaMachine *m, uint32_t key)
{
    const GbaRcProgram *p = m->rc_program;
    uint32_t region = key >> 24;
    if (region != 0x2u && region != 0x3u && (region < 0x08u || region > 0x0Du)) return NULL;
    for (uint32_t h = entry_hash(m, key);; h = (h + 1u) & m->rc_index_mask) {
        uint32_t index = m->rc_index[h];
        if (!index) return NULL;
        const GbaRcEntry *e = &p->entries[index - 1u];
        if (e->key != key) continue;
        if (e->block >= 0 && m->rc_verified[e->block] != m->rc_ram_gen) {
            const GbaRcRamBlock *blk = &p->ram_blocks[e->block];
            if (memcmp(ram_ptr(m, blk->addr), blk->bytes, blk->size) != 0) return NULL;
            m->rc_verified[e->block] = m->rc_ram_gen;
        }
        return e->fn;
    }
}
