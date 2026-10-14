/*
 * Recompiled-program installation, entry lookup and native-code yields
 * (see snes_recomp.h).
 *
 * Lookup goes through an open-addressing hash of the entry keys built at
 * install time (the run loop looks up on every call, return and indirect
 * jump).
 *
 * WRAM code is verified lazily: every write to a 256-byte WRAM page that
 * holds compiled code bumps m->rc_ram_gen (snes_wram_written); a block is
 * compared with live WRAM again only when the generation moved since it was
 * last found intact. Self-modifying code therefore runs natively only
 * while its bytes match the snapshot it was compiled from, and falls back
 * to the interpreter otherwise.
 */
#include "snes_recomp.h"

#include <stdlib.h>
#include <string.h>

#include "snes_sha256.h"

static uint32_t hash_key(uint32_t key)
{
    key ^= key >> 15;
    key *= 0x2C1B3C6Du;
    key ^= key >> 12;
    key *= 0x297A2D39u;
    key ^= key >> 15;
    return key;
}

static void release(SnesMachine *m)
{
    free(m->rc_hash);
    free(m->rc_verified);
    m->rc_hash = NULL;
    m->rc_verified = NULL;
    m->rc_hash_mask = 0;
    memset(m->rc_code_pages, 0, sizeof m->rc_code_pages);
    m->rc_program = NULL;
}

bool snes_rc_install(SnesMachine *m, const SnesRcProgram *program)
{
    release(m);
    if (!program) return true;
    if (!m->rom || m->rom_size != program->rom_size) return false;
    char digest[65];
    snes_sha256_hex(m->rom, m->rom_size, digest);
    if (strcmp(digest, program->rom_sha256) != 0) return false;

    uint32_t size = 16;
    while (size < program->entry_count * 2u) size <<= 1;
    m->rc_hash = malloc(size * sizeof *m->rc_hash);
    if (!m->rc_hash) return false;
    for (uint32_t i = 0; i < size; i++) m->rc_hash[i] = UINT32_MAX;
    m->rc_hash_mask = size - 1u;
    for (uint32_t i = 0; i < program->entry_count; i++) {
        uint32_t slot = hash_key(program->entries[i].key) & m->rc_hash_mask;
        while (m->rc_hash[slot] != UINT32_MAX) slot = (slot + 1u) & m->rc_hash_mask;
        m->rc_hash[slot] = i;
    }

    if (program->ram_block_count) {
        m->rc_verified = calloc(program->ram_block_count, sizeof *m->rc_verified);
        if (!m->rc_verified) {
            release(m);
            return false;
        }
        for (uint32_t b = 0; b < program->ram_block_count; b++) {
            const SnesRcRamBlock *blk = &program->ram_blocks[b];
            for (uint32_t a = blk->wram_offset & ~0xFFu; a < blk->wram_offset + blk->size; a += 0x100u)
                m->rc_code_pages[(a >> 8) & 0x1FFu] = 1;
        }
    }
    m->rc_ram_gen = 1; /* every block starts unverified (generation 0) */
    m->rc_program = program;
    return true;
}

SnesRcFunc snes_rc_lookup(SnesMachine *m, uint32_t key)
{
    const SnesRcProgram *p = m->rc_program;
    /* Compiled with the SA-1's power-on ROM banking; other banking maps
     * other bytes at these addresses. */
    if (m->sa1.present && !m->sa1.mmc_linear) return NULL;
    uint32_t slot = hash_key(key) & m->rc_hash_mask;
    for (;;) {
        uint32_t index = m->rc_hash[slot];
        if (index == UINT32_MAX) return NULL;
        const SnesRcEntry *e = &p->entries[index];
        if (e->key == key) {
            if (e->block >= 0 && m->rc_verified[e->block] != m->rc_ram_gen) {
                const SnesRcRamBlock *blk = &p->ram_blocks[e->block];
                if (memcmp(&m->wram[blk->wram_offset], blk->bytes, blk->size) != 0) return NULL;
                m->rc_verified[e->block] = m->rc_ram_gen;
            }
            return e->fn;
        }
        slot = (slot + 1u) & m->rc_hash_mask;
    }
}

bool snes_rc_sync(SnesCpu *c)
{
    /* The SA-1 returns to its own run loop (snes_sa1.c), which looks at
     * interrupts and the timer. */
    if (c->id) return true;
    SnesMachine *m = SNES_MACHINE(c);
    if (c->cycles >= m->next_event) snes_process_events(m);
    if (c->wai || c->stp) snes_update_attention(m);
    /* An IRQ held off by the I flag keeps next_sync at 0, so this runs after
     * every instruction until the flag clears or the line drops. */
    return (m->attention & 6u) || m->nmi_transition || (m->irq_since != UINT64_MAX && !c->flag_i);
}
