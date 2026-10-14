/*
 * Recompiled-program installation, entry lookup and native-code yields
 * (see nes_recomp.h).
 *
 * Lookup goes through an open-addressing hash of the entry keys built at
 * install time (the run loop looks up on every call, return, indirect jump
 * and page crossing).
 */
#include "nes_recomp.h"

#include <stdlib.h>
#include <string.h>

#include "nes_internal.h"
#include "nes_sha256.h"

static uint32_t hash_key(uint32_t key)
{
    key ^= key >> 15;
    key *= 0x2C1B3C6Du;
    key ^= key >> 12;
    key *= 0x297A2D39u;
    key ^= key >> 15;
    return key;
}

static void release(NesMachine *m)
{
    free(m->rc_hash);
    m->rc_hash = NULL;
    m->rc_hash_mask = 0;
    m->rc_program = NULL;
}

bool nes_rc_install(NesMachine *m, const NesRcProgram *program)
{
    release(m);
    if (!program) return true;
    if (!m->file || m->file_size != program->rom_size) return false;
    char digest[65];
    nes_sha256_hex(m->file, m->file_size, digest);
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
    m->rc_program = program;
    return true;
}

NesRcFunc nes_rc_lookup(NesMachine *m, uint32_t key)
{
    const NesRcProgram *p = m->rc_program;
    if (key == NES_RC_NO_KEY) return NULL;
    uint32_t slot = hash_key(key) & m->rc_hash_mask;
    for (;;) {
        uint32_t index = m->rc_hash[slot];
        if (index == UINT32_MAX) return NULL;
        const NesRcEntry *e = &p->entries[index];
        if (e->key == key) return e->fn;
        slot = (slot + 1u) & m->rc_hash_mask;
    }
}

bool nes_rc_sync(NesCpu *c)
{
    NesMachine *m = NES_MACHINE(c);
    if (c->cycles >= m->next_event) nes_sync(m);
    nes_update_attention(m);
    /* Exactly when the run loop would take an interrupt, or has something
     * else to do. An active IRQ line keeps next_sync at 0 (even while the I
     * flag masks it), so this runs after every instruction until the line
     * is released; whether the interrupt is taken depends on the I flag the
     * instruction sampled, which nes_irq_sampled() reads. */
    return (m->attention & (NES_ATTENTION_HALT | NES_ATTENTION_FRAME | NES_ATTENTION_REMAP)) || m->nmi_pending ||
           nes_irq_sampled(m);
}
