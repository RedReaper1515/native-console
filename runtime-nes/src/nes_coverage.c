/*
 * Where does recompiled code fall back to the interpreter? Counts of
 * interpreted instructions by region, the hottest interpreted addresses,
 * and a trace for nesrecomp --trace:
 *
 *   FILE        entry keys (PRG page << 16 | PC, hex, one per line): where
 *               interpreted execution began after a branch or an interrupt,
 *               or where recompiled code returned to the run loop
 *
 * Keys carry the 8 KiB PRG ROM page mapped at the address because the
 * cartridge switches banks: this is how the recompiler learns which bank
 * was mapped when code was reached through a switch. nesrecomp explores
 * the code after each entry itself. Code outside PRG ROM (RAM) is counted
 * but has no key and is not traced.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes.h"
#include "nes_recomp.h"

typedef struct {
    uint32_t key;   /* stored + 1; 0 = empty */
    uint32_t count; /* interpreted executions */
    uint8_t entry;  /* seen as an entry point */
} CovSlot;

typedef struct NesCoverage {
    CovSlot *slots; /* open addressing */
    uint32_t mask, used;
    bool entry_pending;
    uint64_t rom, ram;
} NesCoverage;

void nes_coverage_enable(NesMachine *m)
{
    if (m->coverage) return;
    NesCoverage *c = calloc(1, sizeof *c);
    if (!c) return;
    c->mask = (1u << 16) - 1u;
    c->slots = calloc(c->mask + 1u, sizeof *c->slots);
    if (!c->slots) {
        free(c);
        return;
    }
    c->entry_pending = true;
    m->coverage = c;
}

void nes_coverage_free(NesMachine *m)
{
    NesCoverage *c = m->coverage;
    if (!c) return;
    free(c->slots);
    free(c);
    m->coverage = NULL;
}

static uint32_t mix(uint32_t k)
{
    k ^= k >> 16;
    k *= 0x7FEB352Du;
    k ^= k >> 15;
    return k;
}

static CovSlot *slot_for(NesCoverage *c, uint32_t key)
{
    uint32_t stored = key + 1u; /* 0 marks an empty slot */
    if (c->used * 2u >= c->mask) {
        uint32_t old_size = c->mask + 1u;
        CovSlot *old = c->slots;
        CovSlot *grown = calloc(old_size * 2u, sizeof *grown);
        if (grown) {
            c->slots = grown;
            c->mask = old_size * 2u - 1u;
            for (uint32_t i = 0; i < old_size; i++) {
                if (!old[i].key) continue;
                uint32_t s = mix(old[i].key) & c->mask;
                while (c->slots[s].key) s = (s + 1u) & c->mask;
                c->slots[s] = old[i];
            }
            free(old);
        }
    }
    uint32_t s = mix(stored) & c->mask;
    while (c->slots[s].key && c->slots[s].key != stored) s = (s + 1u) & c->mask;
    if (!c->slots[s].key) {
        c->slots[s].key = stored;
        c->used++;
    }
    return &c->slots[s];
}

void nes_coverage_entry(NesMachine *m, uint32_t key)
{
    if (key != NES_RC_NO_KEY) slot_for(m->coverage, key)->entry = 1;
}

void nes_coverage_branched(NesMachine *m) { m->coverage->entry_pending = true; }

void nes_coverage_note(NesMachine *m, uint32_t key)
{
    NesCoverage *c = m->coverage;
    if (key == NES_RC_NO_KEY) {
        c->ram++;
        /* Code that continues from RAM into ROM enters ROM afresh. */
        c->entry_pending = true;
        return;
    }
    CovSlot *slot = slot_for(c, key);
    slot->count++;
    if (c->entry_pending) slot->entry = 1;
    c->entry_pending = false;
    c->rom++;
}

void nes_coverage_report(const NesMachine *m)
{
    const NesCoverage *c = m->coverage;
    if (!c) return;
    printf("interpreted instructions: ROM %" PRIu64 ", RAM %" PRIu64 "\n", c->rom, c->ram);
    uint32_t top[12];
    int found = 0;
    for (uint32_t i = 0; i <= c->mask; i++) {
        if (!c->slots[i].key || !c->slots[i].count) continue;
        int k = found;
        while (k > 0 && c->slots[top[k - 1]].count < c->slots[i].count) k--;
        if (k >= 12) continue;
        int last = found < 12 ? found : 11;
        memmove(&top[k + 1], &top[k], (size_t)(last - k) * sizeof top[0]);
        top[k] = i;
        if (found < 12) found++;
    }
    if (!found) return;
    printf("hottest interpreted addresses (PRG page:address):\n");
    for (int k = 0; k < found; k++) {
        uint32_t key = c->slots[top[k]].key - 1u;
        printf("  %02" PRIX32 ":%04X  %" PRIu32 "\n", NES_RC_KEY_PAGE(key), (unsigned)NES_RC_KEY_PC(key),
               c->slots[top[k]].count);
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

bool nes_coverage_write_trace(const NesMachine *m, const char *path)
{
    const NesCoverage *c = m->coverage;
    if (!c) return false;
    uint32_t *keys = malloc((c->used + 1u) * sizeof *keys);
    if (!keys) return false;
    uint32_t n = 0;
    for (uint32_t i = 0; i <= c->mask; i++) {
        if (c->slots[i].key && c->slots[i].entry) keys[n++] = c->slots[i].key - 1u;
    }
    qsort(keys, n, sizeof *keys, cmp_u32);
    FILE *f = fopen(path, "w");
    if (!f) {
        free(keys);
        return false;
    }
    fprintf(f, "# Code entry points seen at run time (PRG page << 16 | PC), for nesrecomp --trace\n");
    for (uint32_t i = 0; i < n; i++) fprintf(f, "%06" PRIX32 "\n", keys[i]);
    bool ok = fclose(f) == 0;
    free(keys);
    return ok;
}
