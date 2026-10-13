/*
 * Where does recompiled code fall back to the interpreter? Counts of
 * interpreted instructions by region, the hottest interpreted addresses,
 * and a trace for snesrecomp --trace:
 *
 *   FILE        entry keys (PC | mode << 24, hex, one per line): where
 *               interpreted execution began after a branch or an interrupt,
 *               or where recompiled code returned to the run loop
 *   FILE.wram   128 KiB WRAM image: final contents, overlaid with the bytes
 *               each interpreted instruction had when it first ran
 *
 * Keys carry the CPU mode because the same bytes decode differently in
 * different register widths. snesrecomp explores the code after each entry
 * itself, and compiles the WRAM code found in the image; the runtime checks
 * those bytes against live WRAM before running them natively.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes.h"
#include "snes_recomp.h"

typedef struct {
    uint32_t key;
    uint32_t count; /* interpreted executions */
    uint8_t entry;  /* seen as an entry point */
} CovSlot;

typedef struct SnesCoverage {
    CovSlot *slots; /* open addressing, key 0 = empty (key 0 is never code: no mode 0 at $00:0000 in practice) */
    uint32_t mask, used;
    bool entry_pending;
    bool sa1_entry_pending;
    uint64_t rom, wram, other;
    uint64_t sa1_rom, sa1_other;
    uint8_t wram_image[SNES_WRAM_SIZE];
    uint8_t wram_captured[SNES_WRAM_SIZE];
} SnesCoverage;

void snes_coverage_enable(SnesMachine *m)
{
    if (m->coverage) return;
    SnesCoverage *c = calloc(1, sizeof *c);
    if (!c) return;
    c->mask = (1u << 16) - 1u;
    c->slots = calloc(c->mask + 1u, sizeof *c->slots);
    if (!c->slots) {
        free(c);
        return;
    }
    c->entry_pending = true;
    c->sa1_entry_pending = true;
    m->coverage = c;
}

void snes_coverage_free(SnesMachine *m)
{
    SnesCoverage *c = m->coverage;
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

static CovSlot *slot_for(SnesCoverage *c, uint32_t key)
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

void snes_coverage_entry(SnesMachine *m, uint32_t key) { slot_for(m->coverage, key)->entry = 1; }

void snes_coverage_branched(SnesMachine *m) { m->coverage->entry_pending = true; }

void snes_coverage_note(SnesMachine *m, uint32_t key)
{
    SnesCoverage *c = m->coverage;
    CovSlot *slot = slot_for(c, key);
    slot->count++;
    if (c->entry_pending) slot->entry = 1;
    c->entry_pending = false;
    const uint32_t pc = SNES_RC_KEY_PC(key);
    int32_t w = snes_rc_wram_offset(pc);
    if (w >= 0) {
        c->wram++;
        /* Capture the instruction's bytes (up to 4) as they were when it ran. */
        for (uint32_t i = 0; i < 4; i++) {
            int32_t o = snes_rc_wram_offset((pc & 0xFF0000u) | ((pc + i) & 0xFFFFu));
            if (o >= 0 && !c->wram_captured[o]) {
                c->wram_image[o] = m->wram[o];
                c->wram_captured[o] = 1;
            }
        }
    } else if (m->page_type[pc >> 12] == SNES_PAGE_ROM) {
        c->rom++;
    } else {
        c->other++;
    }
}

/* SA-1 code in ROM: the S-CPU's view of those addresses is the same ROM. */
static bool sa1_rom_key(uint32_t key)
{
    const uint32_t pc = SNES_RC_KEY_PC(key);
    return (pc & 0x408000u) == 0x008000u || (pc & 0xC00000u) == 0xC00000u;
}

void snes_coverage_sa1_entry(SnesMachine *m, uint32_t key)
{
    if (sa1_rom_key(key)) slot_for(m->coverage, key)->entry = 1;
}

void snes_coverage_sa1_branched(SnesMachine *m) { m->coverage->sa1_entry_pending = true; }

void snes_coverage_sa1_note(SnesMachine *m, uint32_t key)
{
    SnesCoverage *c = m->coverage;
    if (sa1_rom_key(key)) {
        CovSlot *slot = slot_for(c, key);
        slot->count++;
        if (c->sa1_entry_pending) slot->entry = 1;
        c->sa1_rom++;
    } else {
        c->sa1_other++;
    }
    c->sa1_entry_pending = false;
}

void snes_coverage_report(const SnesMachine *m)
{
    const SnesCoverage *c = m->coverage;
    if (!c) return;
    printf("interpreted instructions: ROM %" PRIu64 ", WRAM %" PRIu64 ", other %" PRIu64 "\n", c->rom, c->wram, c->other);
    if (m->sa1.present)
        printf("interpreted SA-1 instructions: ROM %" PRIu64 ", I-RAM/BW-RAM %" PRIu64 "\n", c->sa1_rom, c->sa1_other);
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
    printf("hottest interpreted addresses:\n");
    for (int k = 0; k < found; k++) {
        uint32_t key = c->slots[top[k]].key - 1u;
        printf("  %02" PRIX32 ":%04" PRIX32 " mode %" PRIu32 "  %" PRIu32 "\n", SNES_RC_KEY_PC(key) >> 16,
               SNES_RC_KEY_PC(key) & 0xFFFFu, SNES_RC_KEY_MODE(key), c->slots[top[k]].count);
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

bool snes_coverage_write_trace(const SnesMachine *m, const char *path)
{
    const SnesCoverage *c = m->coverage;
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
    fprintf(f, "# Code entry points seen at run time (PC | mode << 24), for snesrecomp --trace\n");
    for (uint32_t i = 0; i < n; i++) fprintf(f, "%07" PRIX32 "\n", keys[i]);
    fclose(f);
    free(keys);

    char name[1024];
    snprintf(name, sizeof name, "%s.wram", path);
    uint8_t *out = malloc(SNES_WRAM_SIZE);
    if (!out) return false;
    for (uint32_t i = 0; i < SNES_WRAM_SIZE; i++) out[i] = c->wram_captured[i] ? c->wram_image[i] : m->wram[i];
    FILE *w = fopen(name, "wb");
    bool ok = w && fwrite(out, 1, SNES_WRAM_SIZE, w) == SNES_WRAM_SIZE;
    if (w) fclose(w);
    free(out);
    return ok;
}
