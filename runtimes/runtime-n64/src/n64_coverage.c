/*
 * Entry and page recording for traces (see n64_coverage.h).
 *
 * Entries live in an open-addressing table keyed by virtual address. A
 * page snapshot is taken the first time an entry falls in a 4 KiB page of
 * RDRAM whose contents have not been seen with that hash: overlays loaded
 * at the same address at different times give separate snapshots. A page
 * is hashed again only after something wrote to it (n64_coverage_written;
 * while coverage is on, every CPU store goes through the bus so it is
 * seen): hashing on every hit made profiling hot loops crawl.
 *
 * The trace ends with all of RDRAM as the run left it (the F lines).
 *
 * A game's window records the same thing while it is played, lightly
 * (n64_coverage_enable_light): CPU stores keep the RDRAM shortcut, so a
 * page is hashed again only after a DMA wrote to it - which is how code
 * arrives. Only the interpreter records, so it costs compiled code nothing.
 */
#include "n64_coverage.h"
#include "n64_io.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t vaddr, paddr;
    uint64_t hits;
} CovEntry;

typedef struct {
    uint32_t paddr;
    uint64_t hash;
    uint8_t *bytes;
} CovPage;

typedef struct {
    CovEntry *entries;
    uint32_t mask, count;
    CovPage *pages;
    uint32_t page_count, page_cap;
    uint8_t dirty[N64_RDRAM_MAX >> 12]; /* written since its last hash, per page */
    bool light;                         /* CPU stores are not seen (n64_coverage_enable_light) */
} Coverage;

static uint32_t hash32(uint32_t k)
{
    k ^= k >> 16;
    k *= 0x7FEB352Du;
    k ^= k >> 15;
    k *= 0x846CA68Bu;
    k ^= k >> 16;
    return k;
}

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static void enable(N64Machine *m, bool light)
{
    if (m->coverage) return;
    Coverage *cv = calloc(1, sizeof *cv);
    if (!cv) return;
    cv->light = light;
    cv->mask = 4095;
    cv->entries = calloc(cv->mask + 1u, sizeof *cv->entries);
    if (!cv->entries) {
        free(cv);
        return;
    }
    for (uint32_t i = 0; i <= cv->mask; i++) cv->entries[i].vaddr = UINT32_MAX;
    memset(cv->dirty, 1, sizeof cv->dirty);
    m->coverage = cv;
    n64_rdram_update_fast(m);
}

void n64_coverage_enable(N64Machine *m) { enable(m, false); }
void n64_coverage_enable_light(N64Machine *m) { enable(m, true); }

void n64_coverage_written(N64Machine *m, uint32_t paddr, uint32_t size)
{
    Coverage *cv = m->coverage;
    if (!size || paddr >= m->rdram_size) return;
    uint32_t last = paddr + size - 1u;
    if (last >= m->rdram_size) last = m->rdram_size - 1u;
    for (uint32_t p = paddr >> 12; p <= last >> 12; p++) cv->dirty[p] = 1;
}

void n64_coverage_free(N64Machine *m)
{
    Coverage *cv = m->coverage;
    if (!cv) return;
    for (uint32_t i = 0; i < cv->page_count; i++) free(cv->pages[i].bytes);
    free(cv->pages);
    free(cv->entries);
    free(cv);
    m->coverage = NULL;
    n64_rdram_update_fast(m);
}

static void grow(Coverage *cv)
{
    uint32_t old = cv->mask + 1u, size = old * 2u;
    CovEntry *e = calloc(size, sizeof *e);
    if (!e) return;
    for (uint32_t i = 0; i < size; i++) e[i].vaddr = UINT32_MAX;
    for (uint32_t i = 0; i < old; i++) {
        if (cv->entries[i].vaddr == UINT32_MAX) continue;
        uint32_t s = hash32(cv->entries[i].vaddr) & (size - 1u);
        while (e[s].vaddr != UINT32_MAX) s = (s + 1u) & (size - 1u);
        e[s] = cv->entries[i];
    }
    free(cv->entries);
    cv->entries = e;
    cv->mask = size - 1u;
}

static void snapshot(N64Machine *m, Coverage *cv, uint32_t paddr)
{
    uint32_t page = paddr & ~0xFFFu;
    if (page + 0x1000u > m->rdram_size) return;
    uint64_t h = fnv(m->rdram + page, 0x1000u);
    for (uint32_t i = 0; i < cv->page_count; i++) {
        if (cv->pages[i].paddr == page && cv->pages[i].hash == h) return;
    }
    if (cv->page_count == cv->page_cap) {
        uint32_t cap = cv->page_cap ? cv->page_cap * 2u : 64u;
        CovPage *p = realloc(cv->pages, cap * sizeof *p);
        if (!p) return;
        cv->pages = p;
        cv->page_cap = cap;
    }
    uint8_t *bytes = malloc(0x1000u);
    if (!bytes) return;
    memcpy(bytes, m->rdram + page, 0x1000u);
    cv->pages[cv->page_count++] = (CovPage){ page, h, bytes };
}

void n64_coverage_entry(N64Machine *m)
{
    Coverage *cv = m->coverage;
    const uint64_t pc = m->cpu.pc;
    if (!cv || (uint64_t)(int64_t)(int32_t)(uint32_t)pc != pc) return;
    uint32_t paddr;
    if (!n64_cpu_translate_quiet(&m->cpu, pc, &paddr)) return;
    const uint32_t key = (uint32_t)pc;
    uint32_t s = hash32(key) & cv->mask;
    while (cv->entries[s].vaddr != UINT32_MAX && cv->entries[s].vaddr != key) s = (s + 1u) & cv->mask;
    if (cv->entries[s].vaddr == key) {
        cv->entries[s].hits++;
        if (cv->entries[s].paddr == paddr && paddr < m->rdram_size && cv->dirty[paddr >> 12]) {
            cv->dirty[paddr >> 12] = 0;
            snapshot(m, cv, paddr);
        }
        return;
    }
    cv->entries[s] = (CovEntry){ key, paddr, 1 };
    cv->count++;
    if (paddr < m->rdram_size) {
        cv->dirty[paddr >> 12] = 0;
        snapshot(m, cv, paddr);
    }
    if (cv->count * 2u > cv->mask) grow(cv);
}

static const char k_header[] = "# n64recomp trace v1: E vaddr paddr, P paddr bytes, F paddr bytes\n";

static void write_page(FILE *f, char kind, uint32_t paddr, const uint8_t *bytes)
{
    static const char hex[] = "0123456789ABCDEF";
    char line[0x2000 + 1];
    for (uint32_t k = 0; k < 0x1000u; k++) {
        line[2 * k] = hex[bytes[k] >> 4];
        line[2 * k + 1] = hex[bytes[k] & 15u];
    }
    line[0x2000] = '\0';
    fprintf(f, "%c %08X %s\n", kind, paddr, line);
}

/* F: RDRAM as the run leaves it, a page a line, empty pages left out. The
 * P pages are each from their own moment and end at a page boundary; this
 * one image lets n64recomp follow a function across pages, and calls into
 * code the run never reached. */
static void write_image(FILE *f, const N64Machine *m)
{
    for (uint32_t page = 0; page + 0x1000u <= m->rdram_size; page += 0x1000u) {
        const uint8_t *p = m->rdram + page;
        bool empty = true;
        for (uint32_t k = 0; k < 0x1000u && empty; k++) empty = !p[k];
        if (!empty) write_page(f, 'F', page, p);
    }
}

bool n64_coverage_write_trace(N64Machine *m, const char *path)
{
    Coverage *cv = m->coverage;
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(k_header, f);
    if (cv) {
        for (uint32_t i = 0; i <= cv->mask; i++) {
            if (cv->entries[i].vaddr != UINT32_MAX)
                fprintf(f, "E %08X %08X\n", cv->entries[i].vaddr, cv->entries[i].paddr);
        }
        for (uint32_t i = 0; i < cv->page_count; i++) write_page(f, 'P', cv->pages[i].paddr, cv->pages[i].bytes);
        write_image(f, m);
    }
    return fclose(f) == 0;
}

/* ---- a trace that grows from one play session to the next ---- */

/* What a trace file already holds: entries as vaddr:paddr, pages as a hash
 * of their address and bytes. Open addressing; 0 marks an empty slot. */
typedef struct {
    uint64_t *keys;
    uint32_t mask, count;
} KeySet;

static bool keyset_add(KeySet *s, uint64_t key)
{
    if (!key) key = 1;
    if (!s->keys || s->count * 2u > s->mask) {
        const uint32_t size = s->keys ? (s->mask + 1u) * 2u : 4096u;
        uint64_t *keys = calloc(size, sizeof *keys);
        if (!keys) return true; /* out of memory: write the line again rather than lose it */
        for (uint32_t i = 0; s->keys && i <= s->mask; i++) {
            if (!s->keys[i]) continue;
            uint32_t at = (uint32_t)(s->keys[i] * 0x9E3779B97F4A7C15ull >> 40) & (size - 1u);
            while (keys[at]) at = (at + 1u) & (size - 1u);
            keys[at] = s->keys[i];
        }
        free(s->keys);
        s->keys = keys;
        s->mask = size - 1u;
    }
    uint32_t at = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 40) & s->mask;
    while (s->keys[at] && s->keys[at] != key) at = (at + 1u) & s->mask;
    if (s->keys[at]) return false;
    s->keys[at] = key;
    s->count++;
    return true;
}

static uint64_t page_key(uint32_t paddr, const uint8_t *bytes) { return fnv(bytes, 0x1000u) ^ ((uint64_t)paddr << 32); }

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

bool n64_coverage_merge_trace(N64Machine *m, const char *path)
{
    Coverage *cv = m->coverage;
    const size_t path_len = strlen(path), line_cap = 0x2000u + 64u;
    char *tmp = malloc(path_len + 5u), *line = malloc(line_cap);
    uint8_t *bytes = malloc(0x1000u);
    FILE *out = NULL, *in = NULL;
    bool ok = false;
    KeySet seen = { NULL, 0, 0 };
    if (!cv || !tmp || !line || !bytes) goto done;
    memcpy(tmp, path, path_len);
    memcpy(tmp + path_len, ".tmp", 5);
    out = fopen(tmp, "w");
    if (!out) goto done;
    fputs(k_header, out);

    /* What earlier sessions recorded stays, once each. Their images do not:
     * only one moment's RDRAM is a consistent picture, and this session's
     * is written below. */
    in = fopen(path, "r");
    if (in) {
        while (fgets(line, (int)line_cap, in)) {
            if (line[0] == 'E') {
                unsigned va, pa;
                if (sscanf(line + 1, "%x %x", &va, &pa) == 2 && keyset_add(&seen, (uint64_t)va << 32 | pa)) fputs(line, out);
            } else if (line[0] == 'P' && strlen(line) >= 11u + 0x2000u) {
                unsigned pa;
                bool good = sscanf(line + 1, "%x", &pa) == 1;
                for (uint32_t k = 0; good && k < 0x1000u; k++) {
                    const int hi = hex_digit(line[11 + 2 * k]), lo = hex_digit(line[12 + 2 * k]);
                    good = hi >= 0 && lo >= 0;
                    bytes[k] = (uint8_t)(hi << 4 | lo);
                }
                if (good && keyset_add(&seen, page_key(pa, bytes))) fputs(line, out);
            }
        }
        fclose(in);
    }
    for (uint32_t i = 0; i <= cv->mask; i++) {
        const CovEntry *e = &cv->entries[i];
        if (e->vaddr != UINT32_MAX && keyset_add(&seen, (uint64_t)e->vaddr << 32 | e->paddr))
            fprintf(out, "E %08X %08X\n", e->vaddr, e->paddr);
    }
    for (uint32_t i = 0; i < cv->page_count; i++) {
        if (keyset_add(&seen, page_key(cv->pages[i].paddr, cv->pages[i].bytes)))
            write_page(out, 'P', cv->pages[i].paddr, cv->pages[i].bytes);
    }
    write_image(out, m);
    ok = fclose(out) == 0;
    out = NULL;
    if (ok) {
        remove(path); /* rename does not replace a file on Windows */
        ok = rename(tmp, path) == 0;
    }
done:
    if (out) fclose(out);
    if (!ok && tmp) remove(tmp);
    free(seen.keys);
    free(bytes);
    free(line);
    free(tmp);
    return ok;
}

uint32_t n64_coverage_entries(const N64Machine *m)
{
    const Coverage *cv = m->coverage;
    return cv ? cv->count : 0;
}

bool n64_coverage_sees_stores(const N64Machine *m)
{
    const Coverage *cv = m->coverage;
    return cv && !cv->light;
}

static int by_hits(const void *a, const void *b)
{
    const CovEntry *x = a, *y = b;
    return x->hits < y->hits ? 1 : (x->hits > y->hits ? -1 : 0);
}

void n64_coverage_report(N64Machine *m)
{
    Coverage *cv = m->coverage;
    if (!cv) return;
    CovEntry *list = malloc((cv->count ? cv->count : 1u) * sizeof *list);
    if (!list) return;
    uint32_t n = 0;
    for (uint32_t i = 0; i <= cv->mask; i++) {
        if (cv->entries[i].vaddr != UINT32_MAX) list[n++] = cv->entries[i];
    }
    qsort(list, n, sizeof *list, by_hits);
    printf("interpreter entries: %u distinct, %u code pages\n", n, cv->page_count);
    for (uint32_t i = 0; i < n && i < 16; i++)
        printf("  %08X (phys %08X): %" PRIu64 " entries\n", list[i].vaddr, list[i].paddr, list[i].hits);
    free(list);
}
