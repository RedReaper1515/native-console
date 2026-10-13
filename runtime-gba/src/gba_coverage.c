/*
 * Where does recompiled code fall back to the interpreter? Per-region counts
 * of interpreted instructions, the hottest interpreted addresses, and a trace
 * for gbarecomp --trace:
 *
 *   FILE          entry points (address | 1 for Thumb), ROM and RAM: where
 *                 interpreted execution began after a branch or after
 *                 recompiled code returned to the dispatcher
 *   FILE.iwram    32 KiB IWRAM image  } final RAM contents, overlaid with the
 *   FILE.ewram    256 KiB EWRAM image } bytes each interpreted instruction
 *                                       had when it first ran
 *
 * Only entries are listed: gbarecomp explores the straight-line code after
 * each one itself, and every listed key becomes a label, so listing every
 * interpreted address would split functions at every instruction. Returns
 * from exceptions are not entries (an interrupt can land between any two
 * interpreted instructions).
 *
 * gbarecomp compiles the RAM code found in the images; the runtime checks
 * those bytes against live RAM before running it natively.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba.h"

#define ROM_SLOTS (1u << 24) /* halfwords of a 32 MiB ROM space */
#define IWRAM_SLOTS (GBA_IWRAM_SIZE / 2u)
#define EWRAM_SLOTS (GBA_EWRAM_SIZE / 2u)

/* seen[] bits: interpreted as ARM / Thumb, entered as ARM / Thumb. */
#define SEEN_ARM 1u
#define SEEN_THUMB 2u
#define ENTRY_ARM 4u
#define ENTRY_THUMB 8u

typedef struct GbaCoverage {
    uint64_t region[16];
    bool entry_pending; /* the next interpreted instruction is an entry */
    uint8_t *rom_seen;
    uint32_t *rom_count;
    uint8_t iwram_seen[IWRAM_SLOTS];
    uint8_t ewram_seen[EWRAM_SLOTS];
    uint32_t iwram_count[IWRAM_SLOTS];
    uint8_t iwram_image[GBA_IWRAM_SIZE];
    uint8_t ewram_image[GBA_EWRAM_SIZE];
    uint8_t iwram_captured[GBA_IWRAM_SIZE];
    uint8_t ewram_captured[GBA_EWRAM_SIZE];
} GbaCoverage;

void gba_coverage_enable(GbaMachine *m)
{
    if (m->coverage) return;
    GbaCoverage *c = calloc(1, sizeof *c);
    c->rom_seen = calloc(ROM_SLOTS, 1);
    c->rom_count = calloc(ROM_SLOTS, sizeof *c->rom_count);
    c->entry_pending = true;
    m->coverage = c;
}

void gba_coverage_free(GbaMachine *m)
{
    GbaCoverage *c = m->coverage;
    if (!c) return;
    free(c->rom_seen);
    free(c->rom_count);
    free(c);
    m->coverage = NULL;
}

/* The seen[] byte for a code address, or NULL outside ROM and RAM. */
static uint8_t *seen_slot(GbaCoverage *c, uint32_t addr)
{
    switch (addr >> 24) {
    case 0x2: return &c->ewram_seen[(addr & (GBA_EWRAM_SIZE - 1)) >> 1];
    case 0x3: return &c->iwram_seen[(addr & (GBA_IWRAM_SIZE - 1)) >> 1];
    case 0x8: case 0x9: return &c->rom_seen[(addr - 0x08000000u) >> 1];
    default: return NULL;
    }
}

void gba_coverage_entry(GbaMachine *m, uint32_t key)
{
    uint8_t *slot = seen_slot(m->coverage, key & ~1u);
    if (slot) *slot |= (key & 1u) ? ENTRY_THUMB : ENTRY_ARM;
}

void gba_coverage_branched(GbaMachine *m)
{
    m->coverage->entry_pending = true;
}

static void capture(uint8_t *image, uint8_t *captured, const uint8_t *mem, uint32_t off, uint32_t size, uint32_t mask)
{
    for (uint32_t i = 0; i < size; i++) {
        uint32_t o = (off + i) & mask;
        if (!captured[o]) {
            image[o] = mem[o];
            captured[o] = 1;
        }
    }
}

void gba_coverage_note(GbaMachine *m, uint32_t key)
{
    GbaCoverage *c = m->coverage;
    uint32_t addr = key & ~1u;
    uint32_t size = (key & 1u) ? 2u : 4u;
    c->region[(addr >> 24) & 15u]++;
    uint8_t *slot = seen_slot(c, addr);
    bool entry = c->entry_pending;
    c->entry_pending = false;
    if (!slot) return;
    *slot |= (key & 1u) ? SEEN_THUMB : SEEN_ARM;
    if (entry) *slot |= (key & 1u) ? ENTRY_THUMB : ENTRY_ARM;
    switch (addr >> 24) {
    case 0x2:
        capture(c->ewram_image, c->ewram_captured, m->ewram, addr & (GBA_EWRAM_SIZE - 1), size, GBA_EWRAM_SIZE - 1);
        break;
    case 0x3:
        c->iwram_count[(addr & (GBA_IWRAM_SIZE - 1)) >> 1]++;
        capture(c->iwram_image, c->iwram_captured, m->iwram, addr & (GBA_IWRAM_SIZE - 1), size, GBA_IWRAM_SIZE - 1);
        break;
    default:
        c->rom_count[(addr - 0x08000000u) >> 1]++;
        break;
    }
}

static void top_list(const char *title, const uint32_t *counts, uint32_t slots, uint32_t base, const uint8_t *seen)
{
    uint32_t top[12];
    int found = 0;
    for (uint32_t i = 0; i < slots; i++) {
        if (!counts[i]) continue;
        int k = found;
        while (k > 0 && counts[top[k - 1]] < counts[i]) k--;
        if (k >= 12) continue;
        int last = found < 12 ? found : 11;
        memmove(&top[k + 1], &top[k], (size_t)(last - k) * sizeof top[0]);
        top[k] = i;
        if (found < 12) found++;
    }
    if (!found) return;
    printf("hottest interpreted %s addresses:\n", title);
    for (int k = 0; k < found; k++) {
        printf("  %08" PRIX32 " %s %" PRIu32 "\n", base + top[k] * 2u, (seen[top[k]] & SEEN_THUMB) ? "T" : "A",
               counts[top[k]]);
    }
}

void gba_coverage_report(const GbaMachine *m)
{
    const GbaCoverage *c = m->coverage;
    if (!c) return;
    static const char *names[16] = { "BIOS", "?", "EWRAM", "IWRAM", "I/O", "palette", "VRAM", "OAM",
                                     "ROM", "ROM", "ROM", "ROM", "ROM", "ROM", "SRAM", "SRAM" };
    printf("interpreted instructions by region:\n");
    for (int r = 0; r < 16; r++) {
        if (c->region[r]) printf("  %-8s %02X: %" PRIu64 "\n", names[r], r, c->region[r]);
    }
    top_list("ROM", c->rom_count, ROM_SLOTS, 0x08000000u, c->rom_seen);
    top_list("IWRAM", c->iwram_count, IWRAM_SLOTS, 0x03000000u, c->iwram_seen);
}

static bool write_image(const char *path, const char *suffix, const uint8_t *image, const uint8_t *captured,
                        const uint8_t *live, size_t size)
{
    char name[1024];
    snprintf(name, sizeof name, "%s%s", path, suffix);
    uint8_t *out = malloc(size);
    if (!out) return false;
    for (size_t i = 0; i < size; i++) out[i] = captured[i] ? image[i] : live[i];
    FILE *f = fopen(name, "wb");
    bool ok = f && fwrite(out, 1, size, f) == size;
    if (f) fclose(f);
    free(out);
    return ok;
}

static void write_entries(FILE *f, uint32_t base, const uint8_t *seen, uint32_t slots)
{
    for (uint32_t i = 0; i < slots; i++) {
        if (seen[i] & ENTRY_ARM) fprintf(f, "%08" PRIX32 "\n", base + i * 2u);
        if (seen[i] & ENTRY_THUMB) fprintf(f, "%08" PRIX32 "\n", (base + i * 2u) | 1u);
    }
}

bool gba_coverage_write_trace(const GbaMachine *m, const char *path)
{
    const GbaCoverage *c = m->coverage;
    if (!c) return false;
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# Code entry points seen at run time (address | 1 = Thumb), for gbarecomp --trace\n");
    write_entries(f, 0x02000000u, c->ewram_seen, EWRAM_SLOTS);
    write_entries(f, 0x03000000u, c->iwram_seen, IWRAM_SLOTS);
    write_entries(f, 0x08000000u, c->rom_seen, ROM_SLOTS);
    fclose(f);
    return write_image(path, ".iwram", c->iwram_image, c->iwram_captured, m->iwram, GBA_IWRAM_SIZE) &&
           write_image(path, ".ewram", c->ewram_image, c->ewram_captured, m->ewram, GBA_EWRAM_SIZE);
}
