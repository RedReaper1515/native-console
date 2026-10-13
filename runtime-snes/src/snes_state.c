/*
 * Save states: the whole SnesMachine (CPU, WRAM, battery RAM, PPU, APU,
 * DMA, timing and scheduler) as one block behind a header that names the
 * ROM (SHA-256) and the layout (sizeof SnesMachine). A state from another
 * game or another runtime version is refused, never half-applied.
 *
 * Host pointers (the ROM image, the installed program and its lookup
 * tables, diagnostics hooks) are not part of a state: loading keeps the
 * live machine's. Code compiled from WRAM is checked against WRAM again
 * before it next runs.
 */
#include "snes_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes_sha256.h"

#define STATE_MAGIC "NCSNESS1"

typedef struct {
    char magic[8];
    uint32_t machine_size;
    uint32_t rom_size;
    char rom_sha256[64];
} StateHeader;

static void make_header(const SnesMachine *m, StateHeader *h)
{
    char digest[65];
    memset(h, 0, sizeof *h);
    memcpy(h->magic, STATE_MAGIC, 8);
    h->machine_size = (uint32_t)sizeof(SnesMachine);
    h->rom_size = m->rom_size;
    snes_sha256_hex(m->rom, m->rom_size, digest);
    memcpy(h->rom_sha256, digest, 64);
}

/* The pointer and diagnostics fields a state must not carry. */
typedef struct {
    const uint8_t *rom;
    const char *coprocessor;
    const struct SnesRcProgram *rc_program;
    uint32_t *rc_hash;
    uint32_t rc_hash_mask;
    uint8_t rc_code_pages[512];
    uint64_t rc_ram_gen;
    uint64_t *rc_verified;
    struct SnesCoverage *coverage;
    void (*trace)(struct SnesMachine *);
    void *trace_user;
    void (*apu_port_trace)(struct SnesMachine *, unsigned, uint8_t, bool);
} HostFields;

static void take_host(SnesMachine *m, HostFields *h, bool clear)
{
    h->rom = m->rom;
    h->coprocessor = m->cart.coprocessor;
    h->rc_program = m->rc_program;
    h->rc_hash = m->rc_hash;
    h->rc_hash_mask = m->rc_hash_mask;
    memcpy(h->rc_code_pages, m->rc_code_pages, sizeof h->rc_code_pages);
    h->rc_ram_gen = m->rc_ram_gen;
    h->rc_verified = m->rc_verified;
    h->coverage = m->coverage;
    h->trace = m->trace;
    h->trace_user = m->trace_user;
    h->apu_port_trace = m->apu_port_trace;
    if (clear) {
        m->rom = NULL;
        m->cart.coprocessor = NULL;
        m->rc_program = NULL;
        m->rc_hash = NULL;
        m->rc_verified = NULL;
        m->coverage = NULL;
        m->trace = NULL;
        m->trace_user = NULL;
        m->apu_port_trace = NULL;
        m->cpu.user = NULL;
        m->sa1.cpu.user = NULL;
    }
}

static void put_host(SnesMachine *m, const HostFields *h)
{
    m->rom = h->rom;
    m->cart.coprocessor = h->coprocessor;
    m->rc_program = h->rc_program;
    m->rc_hash = h->rc_hash;
    m->rc_hash_mask = h->rc_hash_mask;
    memcpy(m->rc_code_pages, h->rc_code_pages, sizeof m->rc_code_pages);
    m->rc_verified = h->rc_verified;
    m->coverage = h->coverage;
    m->trace = h->trace;
    m->trace_user = h->trace_user;
    m->apu_port_trace = h->apu_port_trace;
    m->cpu.user = m;
    m->sa1.cpu.user = m;
    m->sa1.running = false;
}

bool snes_state_save_file(SnesMachine *m, const char *path)
{
    StateHeader header;
    make_header(m, &header);
    SnesMachine *copy = malloc(sizeof *copy);
    if (!copy) return false;
    memcpy(copy, m, sizeof *copy);
    HostFields host;
    take_host(copy, &host, true);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(&header, sizeof header, 1, f) == 1 && fwrite(copy, sizeof *copy, 1, f) == 1;
    if (f && fclose(f) != 0) ok = false;
    free(copy);
    if (!ok) remove(path);
    return ok;
}

bool snes_state_load_file(SnesMachine *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    StateHeader want, got;
    make_header(m, &want);
    SnesMachine *loaded = malloc(sizeof *loaded);
    bool ok = loaded && fread(&got, sizeof got, 1, f) == 1 && memcmp(&got, &want, sizeof got) == 0 &&
              fread(loaded, sizeof *loaded, 1, f) == 1;
    fclose(f);
    if (!ok) {
        free(loaded);
        return false;
    }
    HostFields host;
    take_host(m, &host, false);
    memcpy(m, loaded, sizeof *m);
    free(loaded);
    put_host(m, &host);
    /* Whatever the state holds in WRAM must be checked before compiled WRAM
     * code runs again. */
    m->rc_ram_gen = host.rc_ram_gen + 1u;
    return true;
}
